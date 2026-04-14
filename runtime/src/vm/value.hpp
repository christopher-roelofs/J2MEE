#pragma once
#include <cstdint>
#include <cstring>

// ─── ObjRef ──────────────────────────────────────────────────────────────────
// A handle into the heap's object table.  0 == null.
// Stored as int32_t in VM slots (safe: table will never exceed 2^31 entries).
using ObjRef = uint32_t;
constexpr ObjRef NULL_REF = 0;

// ─── Slot ────────────────────────────────────────────────────────────────────
// One 32-bit operand-stack / local-variable slot.
// long and double occupy two consecutive slots (low then high word).
// The bytecode verifier (not implemented yet) guarantees type safety;
// here we just store raw bits and cast on demand.
struct Slot {
    int32_t raw = 0;

    static Slot from_int(int32_t v)   { Slot s; s.raw = v; return s; }
    static Slot from_ref(ObjRef r)    { Slot s; s.raw = static_cast<int32_t>(r); return s; }
    static Slot from_float(float f)   { Slot s; std::memcpy(&s.raw, &f, 4); return s; }

    int32_t  as_int()   const { return raw; }
    ObjRef   as_ref()   const { return static_cast<ObjRef>(raw); }
    float    as_float() const { float f; std::memcpy(&f, &raw, 4); return f; }
};

// Two-slot pair used for long / double
struct Slot2 {
    int32_t lo = 0;   // first slot (lower word by JVM convention)
    int32_t hi = 0;   // second slot

    static Slot2 from_long(int64_t v) {
        Slot2 s;
        s.lo = static_cast<int32_t>(v);
        s.hi = static_cast<int32_t>(v >> 32);
        return s;
    }
    static Slot2 from_double(double d) {
        int64_t bits;
        std::memcpy(&bits, &d, 8);
        return from_long(bits);
    }

    int64_t as_long() const {
        return (static_cast<int64_t>(hi) << 32) | static_cast<uint32_t>(lo);
    }
    double as_double() const {
        int64_t bits = as_long();
        double d;
        std::memcpy(&d, &bits, 8);
        return d;
    }
};
