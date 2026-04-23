#pragma once
//
// Cooperative green-thread scheduler, ucontext-based.
//
// Each Java thread runs on its own mmap'd C stack with its own ucontext_t
// and its own std::deque<Frame> for the interpreter to push/pop on. Only
// one thread executes at a time — no locking required, since the interpreter
// and heap are shared — but threads can yield cooperatively at well-defined
// points (Thread.sleep, Object.wait, when a thread exits), letting other
// threads make progress. This replaces the earlier single-threaded-with-
// "pending threads" model, which deadlocked on games that use a loader
// Thread + busy-sleep on the main thread (Bejeweled 3, 3D Bomberman, etc.).

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

#ifdef __EMSCRIPTEN__
// Asyncify-backed fibers replace POSIX ucontext under wasm. The public
// surface of Scheduler stays identical; only the context-switch primitives
// differ (see scheduler.cpp).
#include <emscripten/fiber.h>
#elif defined(__ANDROID__)
// Android NDK removed <ucontext.h>. Each Java thread is a real pthread
// gated by a semaphore so only one runs at a time — cooperatively
// indistinguishable from the ucontext model for the rest of the runtime.
#include <pthread.h>
#include <semaphore.h>
#else
#include <ucontext.h>
#endif

#include "frame.hpp"
#include "heap.hpp"  // for ObjRef, NULL_REF

class VM;

struct JavaThread {
    enum class State { Ready, Running, Sleeping, Waiting, Dead };

    // The java.lang.Thread object handle, if any. The initial MIDlet thread
    // has NULL_REF here; threads spawned from Thread.start() get the real
    // Java Thread ObjRef so Thread.currentThread() and friends resolve.
    ObjRef java_ref = NULL_REF;

    // What this thread runs. Called exactly once on the thread's C stack
    // via swapcontext; when it returns the thread transitions to Dead and
    // yields to the scheduler.
    std::function<void()> entry;

    // Interpreter execution state. Replaces VM::m_call_stack — each thread
    // owns its own frame deque so pushes/pops on one don't disturb another.
    std::deque<Frame> frames;

    // Scheduling
    State     state       = State::Ready;
    uint64_t  wake_at_ms  = 0;        // for Sleeping; absolute timestamp
    ObjRef    wait_on     = NULL_REF; // for Waiting; the monitor object

    // Context-switch state. Under emscripten, Asyncify fibers replace
    // ucontext; each fiber needs its own Asyncify-side stack in addition
    // to its C stack.
#ifdef __EMSCRIPTEN__
    emscripten_fiber_t ctx{};
    std::vector<uint8_t> asyncify_stack;   // fiber's Asyncify stack
#elif defined(__ANDROID__)
    pthread_t thread{};
    sem_t     ctx{};        // semaphore this thread waits on to resume
    bool      started = false;
#else
    ucontext_t ctx{};
#endif
    uint8_t*   stack      = nullptr;
    size_t     stack_size = 0;

    ~JavaThread();
};

class Scheduler {
public:
    Scheduler();
    ~Scheduler();

    // Register a new thread in the Ready state. `java_ref` is the
    // java.lang.Thread ObjRef (or NULL_REF for the initial MIDlet thread).
    // `entry` is called exactly once on the thread's C stack; it typically
    // does `vm.invoke(run, klass, {runnable})` to dispatch the Runnable.
    JavaThread* spawn(ObjRef java_ref, std::function<void()> entry);

    // Drive the scheduler until every spawned thread is Dead. Blocks in
    // short SDL_Delay calls when all ready threads are sleeping so we
    // don't spin a CPU core.
    void run_to_completion();

    // Yield the current thread back to the scheduler; picks another ready
    // thread and swapcontexts to it. Returns when this thread is next
    // scheduled. Safe to call from inside native methods.
    void yield();

    // State transitions for the currently-running thread. Each transitions
    // then yields.
    void sleep_current(uint64_t ms);
    void wait_current(ObjRef monitor);
    // Like wait_current, but with a deadline — wakes either on notify or
    // when `ms` milliseconds elapse. Used by Object.wait(long) for timed waits.
    void wait_current_timed(ObjRef monitor, uint64_t ms);

    // Wake exactly one / all waiters on `monitor`. Safe to call from any
    // thread (i.e. the currently-running one).
    bool notify_one(ObjRef monitor);
    void notify_all(ObjRef monitor);

    JavaThread*       current()       { return m_current; }
    const JavaThread* current() const { return m_current; }

    // Exposed for diagnostic use / the old pending_thread_count() callers.
    size_t ready_count() const;

#ifdef __EMSCRIPTEN__
    // Internal — exposed so the fiber-entry thunk can swap back to the
    // scheduler fiber after a thread finishes. Do not dereference directly
    // from outside the scheduler.
    emscripten_fiber_t* scheduler_ctx_ptr();
#endif

private:
    static void thread_entry(uint32_t hi, uint32_t lo);

    // Picks the next Ready thread, waking any Sleeping thread whose
    // wake_at_ms has passed. Returns nullptr if nothing is ready.
    JavaThread* pick_next();

    std::vector<std::unique_ptr<JavaThread>> m_threads;
    JavaThread* m_current = nullptr;
#ifdef __EMSCRIPTEN__
    emscripten_fiber_t m_scheduler_ctx{};
    std::vector<uint8_t> m_scheduler_asyncify_stack;
    bool m_scheduler_ctx_inited = false;
#elif defined(__ANDROID__)
    sem_t  m_scheduler_ctx{};
    bool   m_scheduler_ctx_inited = false;
public:
    sem_t& scheduler_sem() { return m_scheduler_ctx; }
private:
#else
    ucontext_t  m_scheduler_ctx{};
#endif

    // ucontext passes two uint32_t args to the entry function. We stash the
    // freshly-spawned JavaThread here between makecontext and the first
    // swapcontext so thread_entry can pick it up.
    JavaThread* m_pending_bootstrap = nullptr;
};
