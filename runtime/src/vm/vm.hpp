#pragma once
#include "heap.hpp"
#include "class_def.hpp"
#include "class_loader.hpp"
#include "frame.hpp"
#include "util/jar.hpp"

#include <deque>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ─── JvmException ────────────────────────────────────────────────────────────
// Thrown from the interpreter when the bytecode executes athrow.
// Carries the ObjRef of the Throwable object on the heap.
struct JvmException {
    ObjRef ref;
    std::string message;   // cached for C++ error reporting
    std::string location;  // first frame that annotated the throw site
};

// Thrown when the user closes the window.  This is a C++ exception that
// cannot be caught by Java try-catch blocks — it bypasses the interpreter's
// exception handling entirely.
struct QuitRequest {};

// ─── VM ──────────────────────────────────────────────────────────────────────

class VM {
public:
    explicit VM(const std::string& jar_path);

    // Run the MIDlet: find the main class, call startApp().
    void run(const std::string& midlet_class);

    // ── Execution ─────────────────────────────────────────────────────────────

    // Invoke a method. Args are passed in locals[0..n] order (this first for
    // virtual). Returns 0, 1, or 2 slots (2 for long/double).
    std::vector<Slot> invoke(MethodDef* method, ClassDef* klass,
                             std::vector<Slot> args);
    // Hot-path overload: args already contiguous in memory (typically the
    // caller's operand stack). Avoids per-call std::vector allocation.
    std::vector<Slot> invoke(MethodDef* method, ClassDef* klass,
                             std::span<const Slot> args);

    // ── Class lifecycle ───────────────────────────────────────────────────────

    // Run <clinit> if it hasn't been run yet for this class.
    void initialize_class(ClassDef* klass);

    // ── Object creation ───────────────────────────────────────────────────────

    // Allocate a new instance; does NOT call <init>.
    ObjRef new_object(ClassDef* klass);

    // Allocate + populate a java.lang.String from a C++ string.
    ObjRef new_string(const std::string& utf8);

    // Get the underlying C++ string from a heap String object.
    std::string string_value(ObjRef ref);

    // ── Constant pool resolution (called by interpreter) ─────────────────────

    // Resolve a class reference from the current class's CP.
    ClassDef* resolve_class(const ClassFile& cf, uint16_t cp_idx);

    // Resolve a field reference; also returns the owning class.
    struct FieldRef { ClassDef* klass; FieldDef* field; };
    FieldRef resolve_field(const ClassFile& cf, uint16_t cp_idx);

    // Resolve a method reference (for invokestatic / invokespecial).
    struct MethodRef { ClassDef* klass; MethodDef* method; };
    MethodRef resolve_method(const ClassFile& cf, uint16_t cp_idx);

    // ── Accessors ─────────────────────────────────────────────────────────────

    Heap&        heap()    { return m_heap; }
    ClassLoader& loader()  { return m_loader; }

    // The class representing java/lang/String (interned at startup).
    ClassDef*    string_class()  { return m_string_class; }
    ClassDef*    object_class()  { return m_object_class; }

    // ── Native method registry ────────────────────────────────────────────────
    // Called before run() to register MIDP implementations.
    void register_native(const std::string& cls, const std::string& name,
                         const std::string& desc, NativeFunc fn) {
        m_loader.register_native(cls, name, desc, std::move(fn));
    }

    // Pre-set a static field (creates it if missing). Used to init System.out etc.
    void set_static(const std::string& class_name, const std::string& field_name,
                    const std::string& desc, Slot value);

private:
    JarFile     m_jar;
    Heap        m_heap;
    ClassLoader m_loader;

    ClassDef*   m_string_class = nullptr;
    ClassDef*   m_object_class = nullptr;

    // String intern pool: C++ string → ObjRef of heap String
    std::unordered_map<std::string, ObjRef> m_string_pool;

    // Classes whose <clinit> has been started (to prevent re-entry)
    std::unordered_set<ClassDef*> m_clinit_started;

    // Resolution caches — hot path, avoid per-invoke CP walks + utf8 lookups.
    // Keyed by (ClassFile*, cp_idx) packed into 64 bits.
    std::unordered_map<uint64_t, MethodRef> m_method_cache;
    std::unordered_map<uint64_t, FieldRef>  m_field_cache;
    std::unordered_map<uint64_t, ClassDef*> m_class_cache;

    // Monomorphic inline cache for invokevirtual. Key = (cf_ptr, cp_idx).
    // Value = (last_seen_this_klass, resolved_method). On hit, we skip the
    // linear resolve_virtual walk.
    struct VICacheEntry { ClassDef* klass; MethodDef* method; };
    std::unordered_map<uint64_t, VICacheEntry> m_vi_cache;

public:
    // Accessor for the inline cache (used by interpreter hot path).
    MethodDef* vi_lookup(uint64_t key, ClassDef* actual, MethodDef* fallback) {
        auto it = m_vi_cache.find(key);
        if (it != m_vi_cache.end() && it->second.klass == actual)
            return it->second.method;
        MethodDef* resolved = actual ? actual->resolve_virtual(
                                         fallback->name, fallback->descriptor)
                                     : fallback;
        if (!resolved) resolved = fallback;
        m_vi_cache[key] = {actual, resolved};
        return resolved;
    }
private:

    // Active call stack (frames).
    // std::deque: push_back never invalidates references to existing elements,
    // which is required because exec_frame holds Frame& while pushing new frames.
    std::deque<Frame> m_call_stack;

    // Threads started via Thread.start() that have not yet run.
    // Deferred until after startApp() returns so that object construction
    // (which calls Thread.start() before setting singleton fields) can complete.
    struct PendingThread {
        MethodDef* run_method;
        ClassDef*  run_klass;
        ObjRef     runnable;
        ObjRef     thread_ref;
    };

public:
    // Called by the Thread.start() native to defer thread execution.
    void enqueue_thread(ObjRef thread_ref, ObjRef runnable,
                        MethodDef* run_method, ClassDef* run_klass) {
        m_pending_threads.push_back({run_method, run_klass, runnable, thread_ref});
    }

    // Pop and run one pending thread's run() synchronously. Used by
    // Object.wait() to yield to a waiter-notifier thread in our cooperative
    // single-threaded model. Returns true if a thread ran.
    bool run_next_pending_thread();
    size_t pending_thread_count() const { return m_pending_threads.size(); }

    // The ObjRef of the thread currently executing (set by the drain loop).
    ObjRef current_thread = NULL_REF;

private:
    std::vector<PendingThread> m_pending_threads;

    // ── Internal execution ────────────────────────────────────────────────────
    void exec_frame(Frame& frame);
};
