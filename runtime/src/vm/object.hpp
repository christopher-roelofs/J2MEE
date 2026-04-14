#pragma once
#include "value.hpp"
#include <cstdint>
#include <cstddef>

struct ClassDef;  // forward — defined in class_def.hpp

// ─── HeapObject ──────────────────────────────────────────────────────────────
// Every object on the heap has this header, followed immediately by either:
//   - instance fields:  Slot data[klass->instance_slot_count]
//   - array elements:   int32_t length, then T data[length]
//     where T is Slot (reference/int/float arrays) or int64_t (long arrays)
//
// We never sizeof(HeapObject) and index into it directly; use the accessors.

struct HeapObject {
    ClassDef* klass;       // type; null only for arrays of primitives
    uint32_t  gc_flags;    // low bit = mark (for mark-sweep)
    uint32_t  data_words;  // number of int32_t words following this header

    // ── mark bit helpers ────────────────────────────────────────────────────
    bool is_marked() const { return gc_flags & 1u; }
    void set_mark()        { gc_flags |=  1u; }
    void clear_mark()      { gc_flags &= ~1u; }

    // ── field access (instance objects) ─────────────────────────────────────
    // Fields are laid out as Slots starting right after the header.
    Slot* field_slots() {
        return reinterpret_cast<Slot*>(this + 1);
    }
    const Slot* field_slots() const {
        return reinterpret_cast<const Slot*>(this + 1);
    }
    Slot& field(uint32_t slot_index) {
        return field_slots()[slot_index];
    }

    // ── array access ─────────────────────────────────────────────────────────
    // Array layout (after header):
    //   int32_t length
    //   T       elements[length]
    int32_t& array_length() {
        return *reinterpret_cast<int32_t*>(this + 1);
    }
    int32_t array_length() const {
        return *reinterpret_cast<const int32_t*>(this + 1);
    }
    // Element pointer for Slot-width arrays (reference, int, float)
    Slot* array_slots() {
        return reinterpret_cast<Slot*>(
            reinterpret_cast<int32_t*>(this + 1) + 1);
    }
    // Element pointer for byte/boolean arrays (uint8_t)
    uint8_t* array_bytes() {
        return reinterpret_cast<uint8_t*>(
            reinterpret_cast<int32_t*>(this + 1) + 1);
    }
    // Element pointer for char/short arrays (uint16_t / int16_t)
    uint16_t* array_shorts() {
        return reinterpret_cast<uint16_t*>(
            reinterpret_cast<int32_t*>(this + 1) + 1);
    }
    // Element pointer for long arrays (int64_t, 2 words each)
    int64_t* array_longs() {
        return reinterpret_cast<int64_t*>(
            reinterpret_cast<int32_t*>(this + 1) + 1);
    }
};

// ─── Array element types (mirrors JVM newarray type codes) ──────────────────
enum class ArrayType : uint8_t {
    Boolean = 4,
    Char    = 5,
    Float   = 6,
    Double  = 7,
    Byte    = 8,
    Short   = 9,
    Int     = 10,
    Long    = 11,
    Ref     = 255,  // reference array (anewarray/multianewarray)
};
