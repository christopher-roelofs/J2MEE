#pragma once
#include "value.hpp"
#include "classfile/class_file.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// ─── Forward declarations ────────────────────────────────────────────────────
struct ClassDef;
class  VM;
struct Frame;

// ─── NativeFunc ──────────────────────────────────────────────────────────────
// Signature for native method implementations.
// The native receives the VM (for heap/class access) and the argument slots
// as they appear on the operand stack (this + args for virtual, args for static).
// It pushes its return value(s) onto the stack itself, then returns.
using NativeFunc = std::function<void(VM&, Frame&, std::span<Slot>)>;

// ─── FieldDef ────────────────────────────────────────────────────────────────

struct FieldDef {
    std::string name;
    std::string descriptor;
    uint16_t    access_flags;

    // For instance fields: slot index within the object's field_slots() array.
    // For static fields: index into ClassDef::static_values.
    uint32_t slot_index;

    bool is_static()  const { return access_flags & AccessFlags::STATIC; }
    bool is_ref()     const;   // true if descriptor starts with L or [
    bool is_long()    const { return descriptor == "J"; }
    bool is_double()  const { return descriptor == "D"; }

    // How many Slots this field occupies (1 normally, 2 for long/double)
    uint32_t slot_count() const { return (is_long() || is_double()) ? 2u : 1u; }
};

// ─── MethodDef ───────────────────────────────────────────────────────────────

struct MethodDef {
    std::string name;
    std::string descriptor;
    uint16_t    access_flags;

    ClassDef*   owner;          // the class that declares this method

    // Bytecode (null for native/abstract)
    const CodeAttribute* code = nullptr;

    // Native implementation (null for interpreted)
    NativeFunc native_impl;

    bool is_static()   const { return access_flags & AccessFlags::STATIC; }
    bool is_native()   const { return access_flags & AccessFlags::NATIVE; }
    bool is_abstract() const { return access_flags & AccessFlags::ABSTRACT; }

    // Count of argument slots (not counting 'this' for virtual methods)
    // Derived from the descriptor at link time.
    uint32_t arg_slot_count = 0;

    // Return type category
    enum class RetType { Void, Int, Long, Float, Double, Ref };
    RetType ret_type = RetType::Void;

    // ── Fast-path dispatch cache (set on first invocation) ──────────────────
    // For hot natives we identify the method kind once (by name/desc/class)
    // and cache a direct function pointer. Subsequent calls skip the string
    // compare chain and std::function dispatch entirely.
    //
    // Func returns true if handled; false falls back to the full slow path.
    using FastPathFunc = bool(*)(VM&, Frame&, uint32_t total_slots);
    mutable FastPathFunc fast_path   = nullptr;
    mutable uint8_t      fp_resolved = 0;  // 1 once we've tried to identify
};

// ─── ClassDef ────────────────────────────────────────────────────────────────

struct ClassDef {
    std::string name;          // slash-separated: "java/lang/String"
    ClassDef*   super = nullptr;
    std::vector<ClassDef*> interfaces;

    // ── Fields ───────────────────────────────────────────────────────────────
    // deque for the same reason as methods: stub FieldDefs are appended at
    // runtime from resolve_field, and cached FieldDef* pointers must stay
    // valid across those insertions.
    std::deque<FieldDef> instance_fields;   // this class's own instance fields
    std::deque<FieldDef> static_fields;

    // Storage for static field values.  Indexed by FieldDef::slot_index.
    std::vector<Slot> static_values;
    // Static long/double values (two consecutive slots each — just use twice the
    // index, same as instance layout, but stored here separately for clarity).

    // Total instance slot count (including inherited fields from super).
    uint32_t instance_slot_count = 0;

    // ── Methods ──────────────────────────────────────────────────────────────
    // std::deque: new stub methods may be appended at runtime (when a call
    // resolves to a not-yet-implemented native). vector would reallocate and
    // invalidate MethodDef* pointers held by the CP/IC caches. deque keeps
    // existing element addresses stable across push_back.
    std::deque<MethodDef> methods;

    // ── Resolved constant pool ────────────────────────────────────────────────
    // Kept as the original parsed entries; resolved (class/field/method) refs
    // are cached lazily in the interpreter using a side table.
    const ClassFile* source = nullptr;   // backing parsed class file (owned by ClassLoader)

    // ── State flags ──────────────────────────────────────────────────────────
    bool initialized = false;   // <clinit> has run

    // ── Helpers ──────────────────────────────────────────────────────────────
    MethodDef* find_method(const std::string& name, const std::string& desc);
    const MethodDef* find_method(const std::string& name, const std::string& desc) const;

    FieldDef* find_field(const std::string& name, const std::string& desc);

    // Virtual dispatch: walk super chain until we find the method.
    MethodDef* resolve_virtual(const std::string& name, const std::string& desc);

    bool is_subclass_of(const ClassDef* other) const;
    bool implements(const ClassDef* iface) const;

    Slot& static_slot(uint32_t idx)             { return static_values[idx]; }
    const Slot& static_slot(uint32_t idx) const { return static_values[idx]; }
};

// ─── Descriptor helpers ───────────────────────────────────────────────────────
namespace desc {

// Count argument slots for a method descriptor like "(ILjava/lang/String;J)V"
uint32_t arg_slots(const std::string& descriptor);

// Return type from descriptor
MethodDef::RetType ret_type(const std::string& descriptor);

// Is a field descriptor a reference type?
bool is_ref(const std::string& descriptor);

// Is a field descriptor a long or double?
bool is_wide(const std::string& descriptor);

}  // namespace desc
