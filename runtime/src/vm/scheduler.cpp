#include "scheduler.hpp"
#include "vm.hpp"

#include <SDL2/SDL.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#include <emscripten/fiber.h>
#endif

extern volatile std::sig_atomic_t g_quit_requested;

// ─── Context-switch primitives ───────────────────────────────────────────────
// Native builds use POSIX ucontext. Emscripten builds use Asyncify fibers;
// the wrapper functions below present the same call pattern the rest of this
// file uses so the scheduler body stays single-version.

namespace {

#ifdef __EMSCRIPTEN__

// Per-fiber Asyncify stack. Big enough for the deepest yield-through chain we
// produce (Interpreter → native → emscripten_sleep). 64 KB is plenty.
constexpr size_t kAsyncifyStackSize = 64 * 1024;

void fiber_entry_thunk(void* arg);

void ctx_make(JavaThread& thr) {
    thr.asyncify_stack.resize(kAsyncifyStackSize);
    emscripten_fiber_init(
        &thr.ctx,
        fiber_entry_thunk,
        &thr,
        thr.stack,
        thr.stack_size,
        thr.asyncify_stack.data(),
        thr.asyncify_stack.size());
}

void ctx_swap_to(emscripten_fiber_t& from, emscripten_fiber_t& to) {
    emscripten_fiber_swap(&from, &to);
}

#else

void ctx_swap_to(ucontext_t& from, ucontext_t& to) {
    swapcontext(&from, &to);
}

#endif

} // namespace

// ─── JavaThread ──────────────────────────────────────────────────────────────

JavaThread::~JavaThread() {
    if (stack) munmap(stack, stack_size);
}

// ─── Scheduler ───────────────────────────────────────────────────────────────

Scheduler::Scheduler() = default;
Scheduler::~Scheduler() = default;

static uint64_t now_ms() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Each thread's C stack. Big enough for deep invokevirtual recursion plus
// any intermediate C++ frames the interpreter produces. Games recurse
// moderately; 1 MB is comfortable and cheap on 64-bit.
static constexpr size_t kStackSize = 1 * 1024 * 1024;

// File-static cell for thread_entry to pick up the freshly-spawned JavaThread.
// Scheduler's m_pending_bootstrap gets copied here right before swapcontext.
JavaThread* g_thread_entry_bootstrap = nullptr;

// The currently-running Scheduler. Set by run_to_completion before any fiber
// swap happens, so the fiber-entry thunk can return control to the scheduler
// fiber without needing to chase the VM. VM is single-instance in practice
// (one per process), so a plain global is fine.
static Scheduler* g_scheduler = nullptr;

#ifndef __EMSCRIPTEN__
void Scheduler::thread_entry(uint32_t /*hi*/, uint32_t /*lo*/) {
    // Runs on the new thread's C stack. The Scheduler instance is accessed
    // through VM's global storage — we trust it survives until all threads
    // are dead. (VM owns the scheduler.)
    JavaThread* self = g_thread_entry_bootstrap;
    try {
        if (self->entry) self->entry();
    } catch (const QuitRequest&) {
        g_quit_requested = 1;
    } catch (const JvmException& e) {
        fprintf(stderr, "[thread] uncaught JvmException: %s%s%s\n",
                e.message.c_str(),
                e.location.empty() ? "" : " at ",
                e.location.c_str());
    } catch (const std::exception& e) {
        fprintf(stderr, "[thread] C++ exception: %s\n", e.what());
    }

    self->state = JavaThread::State::Dead;
    // uc_link will swap back to m_scheduler_ctx automatically.
}
#else
// Emscripten fiber entry runs on the fiber's stack; fibers have no automatic
// "return to parent" link, so the thunk must explicitly swap back to the
// scheduler after the Java work finishes. The Scheduler singleton that owns
// the scheduler fiber is reached via the VM global.
void Scheduler::thread_entry(uint32_t, uint32_t) {
    // Unused under emscripten — fiber entry goes through fiber_entry_thunk.
}

namespace {
void fiber_entry_thunk(void* arg) {
    auto* self = static_cast<JavaThread*>(arg);
    try {
        if (self->entry) self->entry();
    } catch (const QuitRequest&) {
        g_quit_requested = 1;
    } catch (const JvmException& e) {
        fprintf(stderr, "[thread] uncaught JvmException: %s%s%s\n",
                e.message.c_str(),
                e.location.empty() ? "" : " at ",
                e.location.c_str());
    } catch (const std::exception& e) {
        fprintf(stderr, "[thread] C++ exception: %s\n", e.what());
    }

    self->state = JavaThread::State::Dead;

    // Return control to the scheduler fiber. run_to_completion set
    // g_scheduler before entering the loop.
    emscripten_fiber_swap(&self->ctx, g_scheduler->scheduler_ctx_ptr());
    // Must never reach here — the fiber is dead.
}
}
#endif

#ifdef __EMSCRIPTEN__
emscripten_fiber_t* Scheduler::scheduler_ctx_ptr() { return &m_scheduler_ctx; }
#endif

JavaThread* Scheduler::spawn(ObjRef java_ref, std::function<void()> entry) {
    auto thr = std::make_unique<JavaThread>();
    thr->java_ref = java_ref;
    thr->entry    = std::move(entry);

    thr->stack = static_cast<uint8_t*>(mmap(
        nullptr, kStackSize, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0));
    if (thr->stack == MAP_FAILED)
        throw std::runtime_error(std::string("scheduler: mmap stack: ") + std::strerror(errno));
    thr->stack_size = kStackSize;

#ifdef __EMSCRIPTEN__
    ctx_make(*thr);
#else
    if (getcontext(&thr->ctx) != 0)
        throw std::runtime_error("scheduler: getcontext failed");
    thr->ctx.uc_stack.ss_sp   = thr->stack;
    thr->ctx.uc_stack.ss_size = thr->stack_size;
    thr->ctx.uc_link          = &m_scheduler_ctx;
    m_pending_bootstrap = thr.get();
    makecontext(&thr->ctx, (void (*)())&Scheduler::thread_entry, 2, 0, 0);
#endif

    JavaThread* raw = thr.get();
    m_threads.push_back(std::move(thr));
    if (std::getenv("J2ME_TRACE_SCHED"))
        fprintf(stderr, "[sched] spawn thread java_ref=%d (%zu total)\n",
                java_ref, m_threads.size());
    return raw;
}

JavaThread* Scheduler::pick_next() {
    uint64_t t = now_ms();
    for (auto& thr : m_threads) {
        if (thr->state == JavaThread::State::Sleeping && t >= thr->wake_at_ms)
            thr->state = JavaThread::State::Ready;
        // Timed waiters also wake on their deadline.
        if (thr->state == JavaThread::State::Waiting
            && thr->wake_at_ms != 0 && t >= thr->wake_at_ms) {
            thr->state      = JavaThread::State::Ready;
            thr->wait_on    = NULL_REF;
            thr->wake_at_ms = 0;
        }
    }
    for (auto& thr : m_threads) {
        if (thr->state == JavaThread::State::Ready) return thr.get();
    }
    return nullptr;
}

void Scheduler::run_to_completion() {
#ifdef __EMSCRIPTEN__
    g_scheduler = this;
    // Turn the current (browser main) thread into a fiber so we can swap to
    // spawned fibers and back. Do this lazily the first time run_to_completion
    // is invoked — VM's main Java thread never calls swapcontext before this.
    if (!m_scheduler_ctx_inited) {
        m_scheduler_asyncify_stack.resize(64 * 1024);
        emscripten_fiber_init_from_current_context(
            &m_scheduler_ctx,
            m_scheduler_asyncify_stack.data(),
            m_scheduler_asyncify_stack.size());
        m_scheduler_ctx_inited = true;
    }
#endif

    while (true) {
        if (g_quit_requested) {
            // Still run pending Dead cleanup so ~JavaThread unmmaps stacks.
            for (auto& t : m_threads) t->state = JavaThread::State::Dead;
            break;
        }

        JavaThread* next = pick_next();
        if (!next) {
            bool any_alive = false;
            uint64_t earliest_wake = UINT64_MAX;
            for (auto& thr : m_threads) {
                if (thr->state != JavaThread::State::Dead) any_alive = true;
                if (thr->state == JavaThread::State::Sleeping)
                    earliest_wake = std::min(earliest_wake, thr->wake_at_ms);
                if (thr->state == JavaThread::State::Waiting
                    && thr->wake_at_ms != 0)
                    earliest_wake = std::min(earliest_wake, thr->wake_at_ms);
            }
            if (!any_alive) break;
            uint64_t t = now_ms();
            uint32_t delay = 10;
            if (earliest_wake != UINT64_MAX && earliest_wake > t)
                delay = (uint32_t)std::min<uint64_t>(earliest_wake - t, 50);
            SDL_Delay(delay);
            continue;
        }

        // Publish the bootstrap pointer so thread_entry can read it on
        // first schedule. No-op for threads that have already started.
        g_thread_entry_bootstrap = next;

        m_current = next;
        next->state = JavaThread::State::Running;
        ctx_swap_to(m_scheduler_ctx, next->ctx);
        m_current = nullptr;

        for (auto it = m_threads.begin(); it != m_threads.end(); ) {
            if ((*it)->state == JavaThread::State::Dead) {
                it = m_threads.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void Scheduler::yield() {
    JavaThread* self = m_current;
    if (!self) return;
    if (self->state == JavaThread::State::Running)
        self->state = JavaThread::State::Ready;
    ctx_swap_to(self->ctx, m_scheduler_ctx);
}

void Scheduler::sleep_current(uint64_t ms) {
    JavaThread* self = m_current;
    if (!self) { SDL_Delay((uint32_t)std::min<uint64_t>(ms, 50)); return; }
    // Cap at 50 ms. Feature-phone games from 2008 pace their code assuming
    // a 10-20 FPS interpreter; honouring a 100 ms Thread.sleep literally on
    // modern hardware turns a 5-second loading phase into a multi-minute
    // stall. The cap gives the sleeping thread control back sooner so
    // heavyweight loaders (Bejeweled 3) complete in reasonable wall time,
    // without losing the cooperative-yield property — other ready threads
    // still get scheduled while we're sleeping.
    uint64_t capped = std::min<uint64_t>(ms, 50);
    if (std::getenv("J2ME_TRACE_SCHED"))
        fprintf(stderr, "[sched t=%d] sleep(%lu ms, capped %lu)\n",
                self->java_ref, (unsigned long)ms, (unsigned long)capped);
    self->state      = JavaThread::State::Sleeping;
    self->wake_at_ms = now_ms() + capped;
    ctx_swap_to(self->ctx, m_scheduler_ctx);
}

void Scheduler::wait_current(ObjRef monitor) {
    JavaThread* self = m_current;
    if (!self) return;
    if (std::getenv("J2ME_TRACE_SCHED"))
        fprintf(stderr, "[sched t=%d] wait(obj=%d)\n", self->java_ref, monitor);
    self->state      = JavaThread::State::Waiting;
    self->wait_on    = monitor;
    self->wake_at_ms = 0;  // untimed; only notify can wake
    ctx_swap_to(self->ctx, m_scheduler_ctx);
}

void Scheduler::wait_current_timed(ObjRef monitor, uint64_t ms) {
    JavaThread* self = m_current;
    if (!self) return;
    // Cap deadline same as Thread.sleep — long timeouts on modern hardware
    // would stall games unnecessarily. Notifies still wake immediately.
    uint64_t capped = std::min<uint64_t>(ms, 50);
    if (std::getenv("J2ME_TRACE_SCHED"))
        fprintf(stderr, "[sched t=%d] wait(obj=%d, %lu ms, capped %lu)\n",
                self->java_ref, monitor,
                (unsigned long)ms, (unsigned long)capped);
    self->state      = JavaThread::State::Waiting;
    self->wait_on    = monitor;
    self->wake_at_ms = now_ms() + capped;
    ctx_swap_to(self->ctx, m_scheduler_ctx);
}

bool Scheduler::notify_one(ObjRef monitor) {
    for (auto& thr : m_threads) {
        if (thr->state == JavaThread::State::Waiting && thr->wait_on == monitor) {
            thr->state   = JavaThread::State::Ready;
            thr->wait_on = NULL_REF;
            return true;
        }
    }
    return false;
}

void Scheduler::notify_all(ObjRef monitor) {
    for (auto& thr : m_threads) {
        if (thr->state == JavaThread::State::Waiting && thr->wait_on == monitor) {
            thr->state   = JavaThread::State::Ready;
            thr->wait_on = NULL_REF;
        }
    }
}

size_t Scheduler::ready_count() const {
    size_t n = 0;
    for (auto& thr : m_threads)
        if (thr->state == JavaThread::State::Ready) ++n;
    return n;
}
