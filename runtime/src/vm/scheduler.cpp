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

extern volatile std::sig_atomic_t g_quit_requested;

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

    if (getcontext(&thr->ctx) != 0)
        throw std::runtime_error("scheduler: getcontext failed");
    thr->ctx.uc_stack.ss_sp   = thr->stack;
    thr->ctx.uc_stack.ss_size = thr->stack_size;
    thr->ctx.uc_link          = &m_scheduler_ctx;

    m_pending_bootstrap = thr.get();
    makecontext(&thr->ctx, (void (*)())&Scheduler::thread_entry, 2, 0, 0);

    JavaThread* raw = thr.get();
    m_threads.push_back(std::move(thr));
    return raw;
}

void Scheduler::thread_entry(uint32_t /*hi*/, uint32_t /*lo*/) {
    // Capture the bootstrap immediately — scheduler may spawn more threads
    // before we yield, clobbering m_pending_bootstrap.
    // NOTE: this runs on the new thread's C stack. The Scheduler instance is
    // accessed through VM's global storage — we trust it survives until all
    // threads are dead. (VM owns the scheduler.)
    extern JavaThread* g_thread_entry_bootstrap;
    JavaThread* self = g_thread_entry_bootstrap;

    // Run the thread's work. Any exception must unwind only on THIS C stack.
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

// File-static cell for thread_entry to pick up the freshly-spawned JavaThread.
// Scheduler's m_pending_bootstrap gets copied here right before swapcontext.
JavaThread* g_thread_entry_bootstrap = nullptr;

JavaThread* Scheduler::pick_next() {
    uint64_t t = now_ms();
    for (auto& thr : m_threads) {
        if (thr->state == JavaThread::State::Sleeping && t >= thr->wake_at_ms)
            thr->state = JavaThread::State::Ready;
    }
    for (auto& thr : m_threads) {
        if (thr->state == JavaThread::State::Ready) return thr.get();
    }
    return nullptr;
}

void Scheduler::run_to_completion() {
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
        swapcontext(&m_scheduler_ctx, &next->ctx);
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
    swapcontext(&self->ctx, &m_scheduler_ctx);
}

void Scheduler::sleep_current(uint64_t ms) {
    JavaThread* self = m_current;
    if (!self) { SDL_Delay((uint32_t)std::min<uint64_t>(ms, 50)); return; }
    self->state      = JavaThread::State::Sleeping;
    self->wake_at_ms = now_ms() + ms;
    swapcontext(&self->ctx, &m_scheduler_ctx);
}

void Scheduler::wait_current(ObjRef monitor) {
    JavaThread* self = m_current;
    if (!self) return;
    self->state   = JavaThread::State::Waiting;
    self->wait_on = monitor;
    swapcontext(&self->ctx, &m_scheduler_ctx);
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
