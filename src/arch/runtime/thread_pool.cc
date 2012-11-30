// Copyright 2010-2012 RethinkDB, all rights reserved.
#include "arch/runtime/thread_pool.hpp"

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#include "arch/barrier.hpp"
#include "arch/runtime/event_queue.hpp"
#include "arch/runtime/runtime.hpp"
#include "errors.hpp"
#include "logger.hpp"

const int SEGV_STACK_SIZE = SIGSTKSZ;

__thread linux_thread_pool_t *linux_thread_pool_t::thread_pool;
__thread int linux_thread_pool_t::thread_id;
__thread linux_thread_t *linux_thread_pool_t::thread;

linux_thread_pool_t::linux_thread_pool_t(int worker_threads, bool _do_set_affinity) :
#ifndef NDEBUG
      coroutine_summary(false),
#endif
      interrupt_message(NULL),
      generic_blocker_pool(NULL),
      n_threads(worker_threads + 1),    // we create an extra utility thread
      do_set_affinity(_do_set_affinity)
{
    rassert(n_threads > 1);             // we want at least one non-utility thread
    rassert(n_threads <= MAX_THREADS);

    int res;

    res = pthread_cond_init(&shutdown_cond, NULL);
    guarantee_xerr(res == 0, res, "Could not create shutdown cond");

    res = pthread_mutex_init(&shutdown_cond_mutex, NULL);
    guarantee_xerr(res == 0, res, "Could not create shutdown cond mutex");
}

linux_thread_message_t *linux_thread_pool_t::set_interrupt_message(linux_thread_message_t *m) {
    linux_thread_message_t *o;
    {
        spinlock_acq_t acq(&thread_pool->interrupt_message_lock);

        o = thread_pool->interrupt_message;
        thread_pool->interrupt_message = m;
    }

    return o;
}

struct thread_data_t {
    thread_barrier_t *barrier;
    linux_thread_pool_t *thread_pool;
    int current_thread;
    linux_thread_message_t *initial_message;
};

void *linux_thread_pool_t::start_thread(void *arg) {
    // Block all signals but `SIGSEGV` (will be unblocked by the event queue in
    // case of poll).
    {
        sigset_t sigmask;
        int res = sigfillset(&sigmask);
        guarantee_err(res == 0, "Could not get a full sigmask");

        res = sigdelset(&sigmask, SIGSEGV);
        guarantee_err(res == 0, "Could not remove SIGSEGV from sigmask");

        res = pthread_sigmask(SIG_SETMASK, &sigmask, NULL);
        guarantee_xerr(res == 0, res, "Could not block signal");
    }

    thread_data_t *tdata = reinterpret_cast<thread_data_t *>(arg);

    // Set thread-local variables
    linux_thread_pool_t::thread_pool = tdata->thread_pool;
    linux_thread_pool_t::thread_id = tdata->current_thread;

    // Use a separate block so that it's very clear how long the thread lives for
    // It's not really necessary, but I like it.
    {
        linux_thread_t local_thread(tdata->thread_pool, tdata->current_thread);
        tdata->thread_pool->threads[tdata->current_thread] = &local_thread;
        linux_thread_pool_t::thread = &local_thread;
        blocker_pool_t *generic_blocker_pool = NULL; // Will only be instantiated by one thread

        /* Install a handler for segmentation faults that just prints a backtrace. If we're
        running under valgrind, we don't install this handler because Valgrind will print the
        backtrace for us. */
#ifndef VALGRIND
        stack_t segv_stack;
        segv_stack.ss_sp = malloc_aligned(SEGV_STACK_SIZE, getpagesize());
        guarantee_err(segv_stack.ss_sp != 0, "malloc failed");
        segv_stack.ss_flags = 0;
        segv_stack.ss_size = SEGV_STACK_SIZE;
        int r = sigaltstack(&segv_stack, NULL);
        guarantee_err(r == 0, "sigaltstack failed");

        struct sigaction action;
        bzero(&action, sizeof(action));
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        action.sa_sigaction = &linux_thread_pool_t::sigsegv_handler;
        r = sigaction(SIGSEGV, &action, NULL);
        guarantee_err(r == 0, "Could not install SEGV handler");
#endif  // VALGRIND

        // First thread should initialize generic_blocker_pool before the start barrier
        if (tdata->initial_message) {
            rassert(tdata->thread_pool->generic_blocker_pool == NULL, "generic_blocker_pool already initialized");
            generic_blocker_pool = new blocker_pool_t(GENERIC_BLOCKER_THREAD_COUNT,
                                                      &local_thread.queue);
            tdata->thread_pool->generic_blocker_pool = generic_blocker_pool;
        }

        // If one thread is allowed to run before another one has finished
        // starting up, then it might try to access an uninitialized part of the
        // unstarted one.
        tdata->barrier->wait();
        rassert(tdata->thread_pool->generic_blocker_pool != NULL,
                "Thread passed start barrier while generic_blocker_pool uninitialized");

        // Prime the pump by calling the initial thread message that was passed to thread_pool::run()
        if (tdata->initial_message) {
            local_thread.message_hub.store_message(tdata->current_thread, tdata->initial_message);
        }

        local_thread.queue.run();

        // If one thread is allowed to delete itself before another one has
        // broken out of its loop, it might delete something that the other thread
        // needed to access.
        tdata->barrier->wait();

#ifndef VALGRIND
        free(segv_stack.ss_sp);
#endif

        // If this thread created the generic blocker pool, clean it up
        if (generic_blocker_pool != NULL) {
            delete generic_blocker_pool;
            tdata->thread_pool->generic_blocker_pool = NULL;
        }

        tdata->thread_pool->threads[tdata->current_thread] = NULL;
        linux_thread_pool_t::thread = NULL;
    }

    delete tdata;
    return NULL;
}

#ifndef NDEBUG
void linux_thread_pool_t::enable_coroutine_summary() {
    coroutine_summary = true;
}
#endif

void linux_thread_pool_t::run_thread_pool(linux_thread_message_t *initial_message) {
    do_shutdown = false;

    // Start child threads
    thread_barrier_t barrier(n_threads + 1);

    for (int i = 0; i < n_threads; i++) {
        thread_data_t *tdata = new thread_data_t();
        tdata->barrier = &barrier;
        tdata->thread_pool = this;
        tdata->current_thread = i;
        // The initial message gets sent to thread zero.
        tdata->initial_message = (i == 0) ? initial_message : NULL;

        int res = pthread_create(&pthreads[i], NULL, &start_thread, tdata);
        guarantee_xerr(res == 0, res, "Could not create thread");

        if (do_set_affinity) {
            // On Apple, the thread affinity API has awful documentation, so we don't even bother.
#ifdef _GNU_SOURCE
            // Distribute threads evenly among CPUs
            int ncpus = get_cpu_count();
            cpu_set_t mask;
            CPU_ZERO(&mask);
            CPU_SET(i % ncpus, &mask);
            res = pthread_setaffinity_np(pthreads[i], sizeof(cpu_set_t), &mask);
            guarantee_xerr(res == 0, res, "Could not set thread affinity");
#endif
        }
    }

    // Mark the main thread (for use in assertions etc.)
    linux_thread_pool_t::thread_id = -1;

    // Set up interrupt handlers

    // Wait for threads to start up so that our interrupt handlers can send messages to the threads.
    // TODO(OSX) Fix the goddamn thread pool.
    barrier.wait();

    // TODO: Should we save and restore previous interrupt handlers? This would
    // be a good thing to do before distributing the RethinkDB IO layer, but it's
    // not really important.

#if __MACH__
    const int ITIMER_USEC = 5000;
#endif

    linux_thread_pool_t::thread_pool = this;   // So signal handlers can find us
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(struct sigaction));
        sa.sa_handler = &linux_thread_pool_t::interrupt_handler;

        int res = sigaction(SIGTERM, &sa, NULL);
        guarantee_err(res == 0, "Could not install TERM handler");

        res = sigaction(SIGINT, &sa, NULL);
        guarantee_err(res == 0, "Could not install INT handler");

        // TODO(OSX) inspect this
#if __MACH__
        sa.sa_handler = &linux_thread_pool_t::alrm_handler;
        res = sigaction(SIGALRM, &sa, NULL);
        guarantee_err(res == 0, "Could not install ALRM handler");

        // TODO(OSX) Here we hard-code the number of seconds.
        struct itimerval value;
        value.it_interval.tv_sec = 0;
        value.it_interval.tv_usec = ITIMER_USEC;
        value.it_value = value.it_interval;
        struct itimerval old_value;
        res = setitimer(ITIMER_REAL, &value, &old_value);
        guarantee_err(res == 0, "setitimer call failed");
        guarantee(old_value.it_value.tv_sec == 0 && old_value.it_value.tv_usec == 0);
        guarantee(old_value.it_interval.tv_sec == 0 && old_value.it_interval.tv_usec == 0);
#endif
    }

    // Wait for order to shut down

    int res = pthread_mutex_lock(&shutdown_cond_mutex);
    guarantee_xerr(res == 0, res, "Could not lock shutdown cond mutex");

    while (!do_shutdown) {   // while loop guards against spurious wakeups
        res = pthread_cond_wait(&shutdown_cond, &shutdown_cond_mutex);
        guarantee_xerr(res == 0, res, "Could not wait for shutdown cond");
    }

    res = pthread_mutex_unlock(&shutdown_cond_mutex);
    guarantee_xerr(res == 0, res, "Could not unlock shutdown cond mutex");

    // Remove interrupt handlers

    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(struct sigaction));
        sa.sa_handler = SIG_IGN;

        res = sigaction(SIGTERM, &sa, NULL);
        guarantee_err(res == 0, "Could not remove TERM handler");

        res = sigaction(SIGINT, &sa, NULL);
        guarantee_err(res == 0, "Could not remove INT handler");

        // TODO(OSX) inspect this
#if __MACH__
        struct itimerval value;
        value.it_interval.tv_sec = 0;
        value.it_interval.tv_usec = 0;
        value.it_value = value.it_interval;
        struct itimerval old_value;
        res = setitimer(ITIMER_REAL, &value, &old_value);
        guarantee_err(res == 0, "setitimer call failed");
        guarantee(old_value.it_interval.tv_sec == 0 && old_value.it_interval.tv_usec == ITIMER_USEC);

        res = sigaction(SIGALRM, &sa, NULL);
        guarantee_err(res == 0, "Could not remove ALRM handler");
#endif
    }
    linux_thread_pool_t::thread_pool = NULL;

#ifndef NDEBUG
    // Save each thread's coroutine counters before shutting down
    std::vector<std::map<std::string, size_t> > coroutine_counts(n_threads);
#endif

    // Shut down child threads
    for (int i = 0; i < n_threads; i++) {
        // Cause child thread to break out of its loop
#ifndef NDEBUG
        threads[i]->initiate_shut_down(&coroutine_counts[i]);
#else
        threads[i]->initiate_shut_down();
#endif
    }

    // Wait for barrier, because it expects n_threads + 1 things to wait.  (Otherwise we'd have to
    // have two barriers, which isn't such a problem, but meh.)
    barrier.wait();

    for (int i = 0; i < n_threads; i++) {
        // Wait for child thread to actually exit

        res = pthread_join(pthreads[i], NULL);
        guarantee_xerr(res == 0, res, "Could not join thread");
    }

#ifndef NDEBUG
    if (coroutine_summary)
    {
        // Combine coroutine counts from each thread, and log the totals
        std::map<std::string, size_t> total_coroutine_counts;
        for (int i = 0; i < n_threads; ++i) {
            for (std::map<std::string, size_t>::iterator j = coroutine_counts[i].begin();
                 j != coroutine_counts[i].end(); ++j) {
                total_coroutine_counts[j->first] += j->second;
            }
        }

        for (std::map<std::string, size_t>::iterator i = total_coroutine_counts.begin();
             i != total_coroutine_counts.end(); ++i) {
            logDBG("%ld coroutines ran with type %s", i->second, i->first.c_str());
        }
    }
#endif
}

// TODO(OSX) We do this here.
#if __MACH__
class alrm_message_t : public linux_thread_message_t {
    virtual void on_thread_switch() {
        timer_itimer_forward_alrm();
        delete this;
    }
};

void linux_thread_pool_t::alrm_handler(int) {
    rassert(linux_thread_pool_t::thread_id == -1, "The interrupt handler was called on the wrong thread.");

    linux_thread_pool_t *self = linux_thread_pool_t::thread_pool;

    for (int i = 0; i < self->n_threads; ++i) {
        self->threads[i]->message_hub.insert_external_message(new alrm_message_t);
    }
}

#endif  // __MACH__

// Note: Maybe we should use a signalfd instead of a signal handler, and then
// there would be no issues with potential race conditions because the signal
// would just be pulled out in the main poll/epoll loop. But as long as this works,
// there's no real reason to change it.
void linux_thread_pool_t::interrupt_handler(int) {
    /* The interrupt handler should run on the main thread, the same thread that
    run() was called on. */
    rassert(linux_thread_pool_t::thread_id == -1, "The interrupt handler was called on the wrong thread.");

    linux_thread_pool_t *self = linux_thread_pool_t::thread_pool;

    /* Set the interrupt message to NULL at the same time as we get it so that
    we don't send the same message twice. This is necessary because it's illegal
    to send the same thread message twice until it has been received the first time
    (because of the intrusive list), and we could hypothetically get two SIGINTs
    in quick succession. */
    linux_thread_message_t *interrupt_msg = self->set_interrupt_message(NULL);

    if (interrupt_msg) {
        self->threads[self->n_threads - 1]->message_hub.insert_external_message(interrupt_msg);
    }
}

void linux_thread_pool_t::sigsegv_handler(int signum, siginfo_t *info, UNUSED void *data) {
    if (signum == SIGSEGV) {
        if (is_coroutine_stack_overflow(info->si_addr)) {
            crash("Callstack overflow in a coroutine");
        } else {
            crash("Segmentation fault from reading the address %p.", info->si_addr);
        }
    } else {
        crash("Unexpected signal: %d\n", signum);
    }
}

void linux_thread_pool_t::shutdown_thread_pool() {
    int res;

    // This will tell the main() thread to tell all the child threads to
    // shut down.

    res = pthread_mutex_lock(&shutdown_cond_mutex);
    guarantee_xerr(res == 0, res, "Could not lock shutdown cond mutex");

    do_shutdown = true;

    res = pthread_cond_signal(&shutdown_cond);
    guarantee_xerr(res == 0, res, "Could not signal shutdown cond");

    res = pthread_mutex_unlock(&shutdown_cond_mutex);
    guarantee_xerr(res == 0, res, "Could not unlock shutdown cond mutex");
}

linux_thread_pool_t::~linux_thread_pool_t() {
    int res;

    res = pthread_cond_destroy(&shutdown_cond);
    guarantee_xerr(res == 0, res, "Could not destroy shutdown cond");

    res = pthread_mutex_destroy(&shutdown_cond_mutex);
    guarantee_xerr(res == 0, res, "Could not destroy shutdown cond mutex");
}

linux_thread_t::linux_thread_t(linux_thread_pool_t *parent_pool, int thread_id)
    : queue(this),
      message_hub(&queue, parent_pool, thread_id),
      timer_handler(&queue),
      do_shutdown(false)
#ifndef NDEBUG
      , coroutine_counts_at_shutdown(NULL)
#endif
{
    // Initialize the mutex which synchronizes access to the do_shutdown variable
    int res = pthread_mutex_init(&do_shutdown_mutex, NULL);
    guarantee_xerr(res == 0, res, "could not initialize do_shutdown_mutex");

    // Watch an eventfd for shutdown notifications
    queue.watch_resource(shutdown_notify_event.get_notify_fd(), poll_event_in, this);
}

linux_thread_t::~linux_thread_t() {

#ifndef NDEBUG
    // Save the coroutine counts before they're deleted, should be ready at shutdown
    rassert(coroutine_counts_at_shutdown != NULL);
    coroutine_counts_at_shutdown->clear();
    coro_runtime.get_coroutine_counts(coroutine_counts_at_shutdown);
#endif

    int res = pthread_mutex_destroy(&do_shutdown_mutex);
    guarantee_xerr(res == 0, res, "could not destroy do_shutdown_mutex");
}

void linux_thread_t::pump() {
    message_hub.push_messages();
}

void linux_thread_t::on_event(int events) {
    // No-op. This is just to make sure that the event queue wakes up
    // so it can shut down.

    if (events != poll_event_in) {
        logERR("Unexpected event mask: %d", events);
    }
}

bool linux_thread_t::should_shut_down() {
    int res = pthread_mutex_lock(&do_shutdown_mutex);
    guarantee_xerr(res == 0, res, "could not lock do_shutdown_mutex");
    bool result = do_shutdown;
    res = pthread_mutex_unlock(&do_shutdown_mutex);
    guarantee_xerr(res == 0, res, "could not unlock do_shutdown_mutex");
    return result;
}

#ifndef NDEBUG
void linux_thread_t::initiate_shut_down(std::map<std::string, size_t> *coroutine_counts) {
#else
void linux_thread_t::initiate_shut_down() {
#endif
    int res = pthread_mutex_lock(&do_shutdown_mutex);
    guarantee_xerr(res == 0, res, "could not lock do_shutdown_mutex");
#ifndef NDEBUG
    coroutine_counts_at_shutdown = coroutine_counts;
#endif
    do_shutdown = true;
    shutdown_notify_event.write(1);
    res = pthread_mutex_unlock(&do_shutdown_mutex);
    guarantee_xerr(res == 0, res, "could not unlock do_shutdown_mutex");
}
