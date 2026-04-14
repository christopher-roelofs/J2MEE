#pragma once
#include "value.hpp"
#include "class_def.hpp"
#include <vector>
#include <stdexcept>

// ─── Frame ───────────────────────────────────────────────────────────────────
// One activation record: a method call in progress.
// Locals and operand stack are fixed-size (from Code attribute max_locals /
// max_stack) so there's no dynamic allocation during execution.

struct Frame {
    MethodDef*  method  = nullptr;
    ClassDef*   klass   = nullptr;   // class that owns the method
    uint32_t    pc      = 0;

    std::vector<Slot> locals;   // size = max_locals
    std::vector<Slot> stack;    // size = max_stack (pre-allocated)
    uint32_t          sp = 0;   // operand stack depth

    Frame() = default;
    Frame(MethodDef* m, ClassDef* k) : method(m), klass(k) {
        if (m && m->code) {
            locals.resize(m->code->max_locals);
            stack.resize(m->code->max_stack);
        } else {
            // Native frame: give it a small stack so natives can push return values
            stack.resize(8);
        }
    }

    // ── Operand stack ─────────────────────────────────────────────────────────

    void push(Slot s) {
        if (sp >= static_cast<uint32_t>(stack.size()))
            throw std::runtime_error("operand stack overflow (sp=" + std::to_string(sp) + ")");
        stack[sp++] = s;
    }
    Slot pop() {
        if (sp == 0) throw std::runtime_error("operand stack underflow (pop)");
        return stack[--sp];
    }
    Slot& peek(uint32_t depth = 0) {
        if (depth >= sp)
            throw std::runtime_error("operand stack underflow (peek depth=" +
                                     std::to_string(depth) + " sp=" + std::to_string(sp) + ")");
        return stack[sp - 1 - depth];
    }

    void push_int(int32_t v)  { push(Slot::from_int(v)); }
    void push_ref(ObjRef r)   { push(Slot::from_ref(r)); }
    void push_float(float f)  { push(Slot::from_float(f)); }

    int32_t pop_int()   { return pop().as_int(); }
    ObjRef  pop_ref()   { return pop().as_ref(); }
    float   pop_float() { return pop().as_float(); }

    // Longs and doubles occupy two stack slots (lo then hi, per JVM spec).
    void push_long(int64_t v) {
        Slot2 s = Slot2::from_long(v);
        push(Slot::from_int(s.lo));
        push(Slot::from_int(s.hi));
    }
    int64_t pop_long() {
        Slot2 s;
        s.hi = pop().as_int();
        s.lo = pop().as_int();
        return s.as_long();
    }
    void push_double(double v) {
        Slot2 s = Slot2::from_double(v);
        push(Slot::from_int(s.lo));
        push(Slot::from_int(s.hi));
    }
    double pop_double() {
        Slot2 s;
        s.hi = pop().as_int();
        s.lo = pop().as_int();
        return s.as_double();
    }

    // ── Long in locals (two consecutive slots) ────────────────────────────────

    void set_long(uint16_t idx, int64_t v) {
        Slot2 s = Slot2::from_long(v);
        locals[idx    ].raw = s.lo;
        locals[idx + 1].raw = s.hi;
    }
    int64_t get_long(uint16_t idx) const {
        Slot2 s;
        s.lo = locals[idx    ].raw;
        s.hi = locals[idx + 1].raw;
        return s.as_long();
    }
    void set_double(uint16_t idx, double v) {
        Slot2 s = Slot2::from_double(v);
        locals[idx    ].raw = s.lo;
        locals[idx + 1].raw = s.hi;
    }
    double get_double(uint16_t idx) const {
        Slot2 s;
        s.lo = locals[idx    ].raw;
        s.hi = locals[idx + 1].raw;
        return s.as_double();
    }
};
