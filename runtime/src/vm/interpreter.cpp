#include "vm.hpp"
#include "frame.hpp"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

// Shared global state from natives.cpp — accessed here for fast-path natives.
extern std::unordered_map<ObjRef, std::string> g_string_buffers;
extern std::unordered_map<ObjRef, std::vector<ObjRef>> g_vectors;

// ─── Opcode table ─────────────────────────────────────────────────────────────
// Only the opcodes we actually see in CLDC 1.1 games.

enum Opcode : uint8_t {
    NOP             = 0x00,
    ACONST_NULL     = 0x01,
    ICONST_M1       = 0x02,
    ICONST_0        = 0x03,
    ICONST_1        = 0x04,
    ICONST_2        = 0x05,
    ICONST_3        = 0x06,
    ICONST_4        = 0x07,
    ICONST_5        = 0x08,
    LCONST_0        = 0x09,
    LCONST_1        = 0x0a,
    FCONST_0        = 0x0b,
    FCONST_1        = 0x0c,
    FCONST_2        = 0x0d,
    DCONST_0        = 0x0e,
    DCONST_1        = 0x0f,
    BIPUSH          = 0x10,
    SIPUSH          = 0x11,
    LDC             = 0x12,
    LDC_W           = 0x13,
    LDC2_W          = 0x14,
    ILOAD           = 0x15,
    LLOAD           = 0x16,
    FLOAD           = 0x17,
    DLOAD           = 0x18,
    ALOAD           = 0x19,
    ILOAD_0         = 0x1a,
    ILOAD_1         = 0x1b,
    ILOAD_2         = 0x1c,
    ILOAD_3         = 0x1d,
    LLOAD_0         = 0x1e,
    LLOAD_1         = 0x1f,
    LLOAD_2         = 0x20,
    LLOAD_3         = 0x21,
    FLOAD_0         = 0x22,
    FLOAD_1         = 0x23,
    FLOAD_2         = 0x24,
    FLOAD_3         = 0x25,
    DLOAD_0         = 0x26,
    DLOAD_1         = 0x27,
    DLOAD_2         = 0x28,
    DLOAD_3         = 0x29,
    ALOAD_0         = 0x2a,
    ALOAD_1         = 0x2b,
    ALOAD_2         = 0x2c,
    ALOAD_3         = 0x2d,
    IALOAD          = 0x2e,
    LALOAD          = 0x2f,
    FALOAD          = 0x30,
    AALOAD          = 0x32,
    BALOAD          = 0x33,
    CALOAD          = 0x34,
    SALOAD          = 0x35,
    ISTORE          = 0x36,
    LSTORE          = 0x37,
    FSTORE          = 0x38,
    DSTORE          = 0x39,
    ASTORE          = 0x3a,
    ISTORE_0        = 0x3b,
    ISTORE_1        = 0x3c,
    ISTORE_2        = 0x3d,
    ISTORE_3        = 0x3e,
    LSTORE_0        = 0x3f,
    LSTORE_1        = 0x40,
    LSTORE_2        = 0x41,
    LSTORE_3        = 0x42,
    FSTORE_0        = 0x43,
    FSTORE_1        = 0x44,
    FSTORE_2        = 0x45,
    FSTORE_3        = 0x46,
    DSTORE_0        = 0x47,
    DSTORE_1        = 0x48,
    DSTORE_2        = 0x49,
    DSTORE_3        = 0x4a,
    ASTORE_0        = 0x4b,
    ASTORE_1        = 0x4c,
    ASTORE_2        = 0x4d,
    ASTORE_3        = 0x4e,
    IASTORE         = 0x4f,
    LASTORE         = 0x50,
    FASTORE         = 0x51,
    AASTORE         = 0x53,
    BASTORE         = 0x54,
    CASTORE         = 0x55,
    SASTORE         = 0x56,
    POP             = 0x57,
    POP2            = 0x58,
    DUP             = 0x59,
    DUP_X1          = 0x5a,
    DUP_X2          = 0x5b,
    DUP2            = 0x5c,
    DUP2_X1         = 0x5d,
    DUP2_X2         = 0x5e,
    SWAP            = 0x5f,
    IADD            = 0x60,
    LADD            = 0x61,
    FADD            = 0x62,
    DADD            = 0x63,
    ISUB            = 0x64,
    LSUB            = 0x65,
    FSUB            = 0x66,
    DSUB            = 0x67,
    IMUL            = 0x68,
    LMUL            = 0x69,
    FMUL            = 0x6a,
    DMUL            = 0x6b,
    IDIV            = 0x6c,
    LDIV            = 0x6d,
    FDIV            = 0x6e,
    DDIV            = 0x6f,
    IREM            = 0x70,
    LREM            = 0x71,
    FREM            = 0x72,
    DREM            = 0x73,
    INEG            = 0x74,
    LNEG            = 0x75,
    FNEG            = 0x76,
    DNEG            = 0x77,
    ISHL            = 0x78,
    LSHL            = 0x79,
    ISHR            = 0x7a,
    LSHR            = 0x7b,
    IUSHR           = 0x7c,
    LUSHR           = 0x7d,
    IAND            = 0x7e,
    LAND            = 0x7f,
    IOR             = 0x80,
    LOR             = 0x81,
    IXOR            = 0x82,
    LXOR            = 0x83,
    IINC            = 0x84,
    I2L             = 0x85,
    I2F             = 0x86,
    I2D             = 0x87,
    L2I             = 0x88,
    L2F             = 0x89,
    L2D             = 0x8a,
    F2I             = 0x8b,
    F2L             = 0x8c,
    F2D             = 0x8d,
    D2I             = 0x8e,
    D2L             = 0x8f,
    D2F             = 0x90,
    I2B             = 0x91,
    I2C             = 0x92,
    I2S             = 0x93,
    LCMP            = 0x94,
    FCMPL           = 0x95,
    FCMPG           = 0x96,
    DCMPL           = 0x97,
    DCMPG           = 0x98,
    IFEQ            = 0x99,
    IFNE            = 0x9a,
    IFLT            = 0x9b,
    IFGE            = 0x9c,
    IFGT            = 0x9d,
    IFLE            = 0x9e,
    IF_ICMPEQ       = 0x9f,
    IF_ICMPNE       = 0xa0,
    IF_ICMPLT       = 0xa1,
    IF_ICMPGE       = 0xa2,
    IF_ICMPGT       = 0xa3,
    IF_ICMPLE       = 0xa4,
    IF_ACMPEQ       = 0xa5,
    IF_ACMPNE       = 0xa6,
    GOTO            = 0xa7,
    TABLESWITCH     = 0xaa,
    LOOKUPSWITCH    = 0xab,
    IRETURN         = 0xac,
    LRETURN         = 0xad,
    FRETURN         = 0xae,
    DRETURN         = 0xaf,
    ARETURN         = 0xb0,
    RETURN          = 0xb1,
    GETSTATIC       = 0xb2,
    PUTSTATIC       = 0xb3,
    GETFIELD        = 0xb4,
    PUTFIELD        = 0xb5,
    INVOKEVIRTUAL   = 0xb6,
    INVOKESPECIAL   = 0xb7,
    INVOKESTATIC    = 0xb8,
    INVOKEINTERFACE = 0xb9,
    NEW             = 0xbb,
    NEWARRAY        = 0xbc,
    ANEWARRAY       = 0xbd,
    ARRAYLENGTH     = 0xbe,
    ATHROW          = 0xbf,
    CHECKCAST       = 0xc0,
    INSTANCEOF      = 0xc1,
    MONITORENTER    = 0xc2,
    MONITOREXIT     = 0xc3,
    WIDE            = 0xc4,
    MULTIANEWARRAY  = 0xc5,
    IFNULL          = 0xc6,
    IFNONNULL       = 0xc7,
};

// ─── Bytecode read helpers ────────────────────────────────────────────────────

static inline uint8_t  bc_u1(const uint8_t* c, uint32_t pc) { return c[pc]; }
static inline int8_t   bc_s1(const uint8_t* c, uint32_t pc) { return static_cast<int8_t>(c[pc]); }
static inline uint16_t bc_u2(const uint8_t* c, uint32_t pc) {
    return static_cast<uint16_t>((c[pc] << 8) | c[pc+1]);
}
static inline int16_t  bc_s2(const uint8_t* c, uint32_t pc) {
    return static_cast<int16_t>(bc_u2(c, pc));
}
static inline int32_t  bc_s4(const uint8_t* c, uint32_t pc) {
    return (static_cast<int32_t>(c[pc  ]) << 24) |
           (static_cast<int32_t>(c[pc+1]) << 16) |
           (static_cast<int32_t>(c[pc+2]) <<  8) |
            static_cast<int32_t>(c[pc+3]);
}

// ─── LDC helper ───────────────────────────────────────────────────────────────

static void exec_ldc(VM& vm, Frame& f, const ClassFile& cf, uint16_t cp_idx) {
    const auto& entry = cf.constant_pool.at(cp_idx);
    if (auto* v = std::get_if<CpInteger>(&entry)) {
        f.push_int(v->value);
    } else if (auto* v = std::get_if<CpFloat>(&entry)) {
        f.push_float(v->value);
    } else if (auto* v = std::get_if<CpString>(&entry)) {
        std::string s = cf.utf8(v->string_index);
        f.push_ref(vm.new_string(s));
    } else if (auto* v = std::get_if<CpLong>(&entry)) {
        f.push_long(v->value);
    } else if (auto* v = std::get_if<CpDouble>(&entry)) {
        f.push_double(v->value);
    } else {
        throw std::runtime_error("LDC: unsupported CP type at index " +
                                 std::to_string(cp_idx));
    }
}

// ─── invoke helper ────────────────────────────────────────────────────────────
// Pop (arg_count) slots from f.stack into a vector, then call vm.invoke().
// Returns the optional return slot which the caller pushes.

// ─── Fast-path native implementations ────────────────────────────────────────
// Each returns true if handled in-place on f's stack. The dispatcher caches
// a pointer to the matching function on MethodDef so subsequent calls go
// through no string compares.

static bool fp_object_init(VM&, Frame& f, uint32_t) {
    f.sp -= 1; return true;
}
static bool fp_currentTimeMillis(VM&, Frame& f, uint32_t) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    f.push_long((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    return true;
}
static bool fp_math_min_ii(VM&, Frame& f, uint32_t) {
    int32_t b = f.pop_int(), a = f.pop_int();
    f.push_int(a < b ? a : b); return true;
}
static bool fp_math_max_ii(VM&, Frame& f, uint32_t) {
    int32_t b = f.pop_int(), a = f.pop_int();
    f.push_int(a > b ? a : b); return true;
}
static bool fp_math_abs_i(VM&, Frame& f, uint32_t) {
    int32_t a = f.pop_int();
    f.push_int(a < 0 ? -a : a); return true;
}
static bool fp_math_abs_j(VM&, Frame& f, uint32_t) {
    int64_t a = f.pop_long();
    f.push_long(a < 0 ? -a : a); return true;
}
static bool fp_math_min_jj(VM&, Frame& f, uint32_t) {
    int64_t b = f.pop_long(), a = f.pop_long();
    f.push_long(a < b ? a : b); return true;
}
static bool fp_math_max_jj(VM&, Frame& f, uint32_t) {
    int64_t b = f.pop_long(), a = f.pop_long();
    f.push_long(a > b ? a : b); return true;
}
static bool fp_math_sqrt(VM&, Frame& f, uint32_t) {
    f.push_double(std::sqrt(f.pop_double())); return true;
}
static bool fp_math_sin(VM&, Frame& f, uint32_t) {
    f.push_double(std::sin(f.pop_double())); return true;
}
static bool fp_math_cos(VM&, Frame& f, uint32_t) {
    f.push_double(std::cos(f.pop_double())); return true;
}
static bool fp_vector_size(VM&, Frame& f, uint32_t) {
    ObjRef self = f.pop_ref();
    auto it = g_vectors.find(self);
    f.push_int(it == g_vectors.end() ? 0 : (int32_t)it->second.size());
    return true;
}
static bool fp_vector_elementAt(VM&, Frame& f, uint32_t) {
    int32_t idx = f.pop_int();
    ObjRef self = f.pop_ref();
    auto it = g_vectors.find(self);
    if (it != g_vectors.end() && idx >= 0 && idx < (int32_t)it->second.size())
        f.push_ref(it->second[idx]);
    else
        f.push_ref(NULL_REF);
    return true;
}
static bool fp_vector_isEmpty(VM&, Frame& f, uint32_t) {
    ObjRef self = f.pop_ref();
    auto it = g_vectors.find(self);
    f.push_int(it == g_vectors.end() || it->second.empty() ? 1 : 0);
    return true;
}
static bool fp_vector_addElement(VM&, Frame& f, uint32_t) {
    ObjRef elem = f.pop_ref();
    ObjRef self = f.pop_ref();
    g_vectors[self].push_back(elem);
    return true;
}

// Tried-once-then-fail marker (any non-null value that we can compare against)
static bool fp_none(VM&, Frame&, uint32_t) { return false; }

// First-time dispatch: identify the matching fast path once, cache it on
// the MethodDef. Returns the cached pointer (fp_none if no match).
static MethodDef::FastPathFunc identify_fast_path(MethodDef* method, ClassDef* klass) {
    if (!method->is_native()) return fp_none;
    const std::string& cn = klass->name;
    const std::string& mn = method->name;
    const std::string& d  = method->descriptor;

    if (cn == "java/lang/Object" && mn == "<init>" && d == "()V")
        return fp_object_init;
    if (cn == "java/lang/System" && mn == "currentTimeMillis" && d == "()J")
        return fp_currentTimeMillis;

    if (cn == "java/lang/Math") {
        if (mn == "min" && d == "(II)I") return fp_math_min_ii;
        if (mn == "max" && d == "(II)I") return fp_math_max_ii;
        if (mn == "abs" && d == "(I)I")  return fp_math_abs_i;
        if (mn == "abs" && d == "(J)J")  return fp_math_abs_j;
        if (mn == "min" && d == "(JJ)J") return fp_math_min_jj;
        if (mn == "max" && d == "(JJ)J") return fp_math_max_jj;
        if (mn == "sqrt" && d == "(D)D") return fp_math_sqrt;
        if (mn == "sin"  && d == "(D)D") return fp_math_sin;
        if (mn == "cos"  && d == "(D)D") return fp_math_cos;
    }

    if (cn == "java/util/Vector") {
        if (mn == "size"       && d == "()I")                       return fp_vector_size;
        if (mn == "elementAt"  && d == "(I)Ljava/lang/Object;")     return fp_vector_elementAt;
        if (mn == "isEmpty"    && d == "()Z")                       return fp_vector_isEmpty;
        if (mn == "addElement" && d == "(Ljava/lang/Object;)V")     return fp_vector_addElement;
    }

    return fp_none;
}

static inline bool fast_path_native(VM& vm, Frame& f,
                                    MethodDef* method, ClassDef* klass,
                                    uint32_t total_slots) {
    if (__builtin_expect(method->fp_resolved == 0, 0)) {
        method->fast_path   = identify_fast_path(method, klass);
        method->fp_resolved = 1;
    }
    if (method->fast_path == fp_none) return false;
    return method->fast_path(vm, f, total_slots);
}

static void do_invoke(VM& vm, Frame& f,
                      MethodDef* method, ClassDef* klass,
                      uint32_t total_slots) {
    if (fast_path_native(vm, f, method, klass, total_slots))
        return;

    // Args are already contiguous on the operand stack at positions
    // [sp - total_slots .. sp). Pass as a span, skip the std::vector alloc.
    // For total_slots==0 the span is empty; f.stack may also be empty, in
    // which case &stack[0] is UB — hand in a null pointer instead.
    Slot* args_ptr = total_slots ? &f.stack[f.sp - total_slots] : nullptr;
    // vm.invoke may re-enter the interpreter and push new frames, but our
    // caller's frame won't be relocated (m_call_stack uses std::deque which
    // keeps pointers stable across push_back/pop_back).
    auto result = vm.invoke(method, klass,
                            std::span<const Slot>(args_ptr, total_slots));
    f.sp -= total_slots;
    for (auto& s : result) f.push(s);
}

// ─── Main execution loop ──────────────────────────────────────────────────────

bool g_trace = false;

// Heartbeat sampling: if J2ME_HEARTBEAT=<N> is set in the environment, print
// the executing class.method every N interpreter dispatches. Cheap way to
// find infinite Java-level loops when no natives fire. 0 = disabled.
uint64_t g_heartbeat_insns   = 0;
uint64_t g_heartbeat_counter = 0;

void VM::exec_frame(Frame& f) {
    // Use the method's OWNING class for constant pool resolution — the bytecode
    // was compiled against that class's cp, not the runtime type's.
    ClassDef* cp_klass = (f.method && f.method->owner) ? f.method->owner : f.klass;
    const ClassFile* cf_ptr  = cp_klass ? cp_klass->source : nullptr;
    const uint8_t*   code    = f.method->code->code.data();
    const auto&      ex_tbl  = f.method->code->exception_table;

    auto frame_loc = [&]() -> std::string {
        return std::string(f.klass ? f.klass->name : "?") + "." +
               (f.method ? f.method->name : "?") + "@" + std::to_string(f.pc > 0 ? f.pc-1 : 0) +
               " sp=" + std::to_string(f.sp);
    };

    // ── Direct-threaded dispatch ──────────────────────────────────────────────
    // Opcode → label address table. Label addresses via GCC &&label extension.
    // Built lazily on first entry; thereafter fetch is a single indexed load +
    // indirect branch, eliminating the switch's range check and case-table
    // lookup in hot loops.
    static const void* dispatch_table[256];
    static bool dispatch_init = false;
    if (__builtin_expect(!dispatch_init, 0)) {
        for (int i = 0; i < 256; ++i) dispatch_table[i] = &&L_default;
        dispatch_table[NOP] = &&L_NOP;
        dispatch_table[ACONST_NULL] = &&L_ACONST_NULL;
        dispatch_table[ICONST_M1] = &&L_ICONST_M1;
        dispatch_table[ICONST_0]  = &&L_ICONST_0;
        dispatch_table[ICONST_1]  = &&L_ICONST_1;
        dispatch_table[ICONST_2]  = &&L_ICONST_2;
        dispatch_table[ICONST_3]  = &&L_ICONST_3;
        dispatch_table[ICONST_4]  = &&L_ICONST_4;
        dispatch_table[ICONST_5]  = &&L_ICONST_5;
        dispatch_table[LCONST_0]  = &&L_LCONST_0;
        dispatch_table[LCONST_1]  = &&L_LCONST_1;
        dispatch_table[FCONST_0]  = &&L_FCONST_0;
        dispatch_table[FCONST_1]  = &&L_FCONST_1;
        dispatch_table[FCONST_2]  = &&L_FCONST_2;
        dispatch_table[DCONST_0]  = &&L_DCONST_0;
        dispatch_table[DCONST_1]  = &&L_DCONST_1;
        dispatch_table[BIPUSH]    = &&L_BIPUSH;
        dispatch_table[SIPUSH]    = &&L_SIPUSH;
        dispatch_table[LDC]       = &&L_LDC;
        dispatch_table[LDC_W]     = &&L_LDC_W;
        dispatch_table[LDC2_W]    = &&L_LDC_W;  // share
        dispatch_table[ILOAD]     = &&L_ILOAD;
        dispatch_table[LLOAD]     = &&L_LLOAD;
        dispatch_table[FLOAD]     = &&L_ILOAD;  // share with ILOAD
        dispatch_table[DLOAD]     = &&L_LLOAD;
        dispatch_table[ALOAD]     = &&L_ALOAD;
        dispatch_table[ILOAD_0]   = &&L_ILOAD_0; dispatch_table[FLOAD_0] = &&L_ILOAD_0;
        dispatch_table[ILOAD_1]   = &&L_ILOAD_1; dispatch_table[FLOAD_1] = &&L_ILOAD_1;
        dispatch_table[ILOAD_2]   = &&L_ILOAD_2; dispatch_table[FLOAD_2] = &&L_ILOAD_2;
        dispatch_table[ILOAD_3]   = &&L_ILOAD_3; dispatch_table[FLOAD_3] = &&L_ILOAD_3;
        dispatch_table[LLOAD_0]   = &&L_LLOAD_0; dispatch_table[DLOAD_0] = &&L_LLOAD_0;
        dispatch_table[LLOAD_1]   = &&L_LLOAD_1; dispatch_table[DLOAD_1] = &&L_LLOAD_1;
        dispatch_table[LLOAD_2]   = &&L_LLOAD_2; dispatch_table[DLOAD_2] = &&L_LLOAD_2;
        dispatch_table[LLOAD_3]   = &&L_LLOAD_3; dispatch_table[DLOAD_3] = &&L_LLOAD_3;
        dispatch_table[ALOAD_0]   = &&L_ALOAD_0;
        dispatch_table[ALOAD_1]   = &&L_ALOAD_1;
        dispatch_table[ALOAD_2]   = &&L_ALOAD_2;
        dispatch_table[ALOAD_3]   = &&L_ALOAD_3;
        dispatch_table[IALOAD]    = &&L_IALOAD; dispatch_table[FALOAD] = &&L_IALOAD;
        dispatch_table[LALOAD]    = &&L_LALOAD;
        dispatch_table[AALOAD]    = &&L_AALOAD;
        dispatch_table[BALOAD]    = &&L_BALOAD;
        dispatch_table[CALOAD]    = &&L_CALOAD;
        dispatch_table[SALOAD]    = &&L_SALOAD;
        dispatch_table[ISTORE]    = &&L_ISTORE; dispatch_table[FSTORE] = &&L_ISTORE;
        dispatch_table[LSTORE]    = &&L_LSTORE; dispatch_table[DSTORE] = &&L_LSTORE;
        dispatch_table[ASTORE]    = &&L_ASTORE;
        dispatch_table[ISTORE_0]  = &&L_ISTORE_0; dispatch_table[FSTORE_0] = &&L_ISTORE_0;
        dispatch_table[ISTORE_1]  = &&L_ISTORE_1; dispatch_table[FSTORE_1] = &&L_ISTORE_1;
        dispatch_table[ISTORE_2]  = &&L_ISTORE_2; dispatch_table[FSTORE_2] = &&L_ISTORE_2;
        dispatch_table[ISTORE_3]  = &&L_ISTORE_3; dispatch_table[FSTORE_3] = &&L_ISTORE_3;
        dispatch_table[LSTORE_0]  = &&L_LSTORE_0; dispatch_table[DSTORE_0] = &&L_LSTORE_0;
        dispatch_table[LSTORE_1]  = &&L_LSTORE_1; dispatch_table[DSTORE_1] = &&L_LSTORE_1;
        dispatch_table[LSTORE_2]  = &&L_LSTORE_2; dispatch_table[DSTORE_2] = &&L_LSTORE_2;
        dispatch_table[LSTORE_3]  = &&L_LSTORE_3; dispatch_table[DSTORE_3] = &&L_LSTORE_3;
        dispatch_table[ASTORE_0]  = &&L_ASTORE_0;
        dispatch_table[ASTORE_1]  = &&L_ASTORE_1;
        dispatch_table[ASTORE_2]  = &&L_ASTORE_2;
        dispatch_table[ASTORE_3]  = &&L_ASTORE_3;
        dispatch_table[IASTORE]   = &&L_IASTORE; dispatch_table[FASTORE] = &&L_IASTORE;
        dispatch_table[LASTORE]   = &&L_LASTORE;
        dispatch_table[AASTORE]   = &&L_AASTORE;
        dispatch_table[BASTORE]   = &&L_BASTORE;
        dispatch_table[CASTORE]   = &&L_CASTORE;
        dispatch_table[SASTORE]   = &&L_SASTORE;
        dispatch_table[POP]       = &&L_POP;
        dispatch_table[POP2]      = &&L_POP2;
        dispatch_table[DUP]       = &&L_DUP;
        dispatch_table[DUP_X1]    = &&L_DUP_X1;
        dispatch_table[DUP_X2]    = &&L_DUP_X2;
        dispatch_table[DUP2]      = &&L_DUP2;
        dispatch_table[DUP2_X1]   = &&L_DUP2_X1;
        dispatch_table[DUP2_X2]   = &&L_DUP2_X2;
        dispatch_table[SWAP]      = &&L_SWAP;
        dispatch_table[IADD]      = &&L_IADD;
        dispatch_table[ISUB]      = &&L_ISUB;
        dispatch_table[IMUL]      = &&L_IMUL;
        dispatch_table[IDIV]      = &&L_IDIV;
        dispatch_table[IREM]      = &&L_IREM;
        dispatch_table[INEG]      = &&L_INEG;
        dispatch_table[ISHL]      = &&L_ISHL;
        dispatch_table[ISHR]      = &&L_ISHR;
        dispatch_table[IUSHR]     = &&L_IUSHR;
        dispatch_table[IAND]      = &&L_IAND;
        dispatch_table[IOR]       = &&L_IOR;
        dispatch_table[IXOR]      = &&L_IXOR;
        dispatch_table[FADD]      = &&L_FADD;
        dispatch_table[FSUB]      = &&L_FSUB;
        dispatch_table[FMUL]      = &&L_FMUL;
        dispatch_table[FDIV]      = &&L_FDIV;
        dispatch_table[FREM]      = &&L_FREM;
        dispatch_table[FNEG]      = &&L_FNEG;
        dispatch_table[DADD]      = &&L_DADD;
        dispatch_table[DSUB]      = &&L_DSUB;
        dispatch_table[DMUL]      = &&L_DMUL;
        dispatch_table[DDIV]      = &&L_DDIV;
        dispatch_table[DREM]      = &&L_DREM;
        dispatch_table[DNEG]      = &&L_DNEG;
        dispatch_table[LADD]      = &&L_LADD;
        dispatch_table[LSUB]      = &&L_LSUB;
        dispatch_table[LMUL]      = &&L_LMUL;
        dispatch_table[LDIV]      = &&L_LDIV;
        dispatch_table[LREM]      = &&L_LREM;
        dispatch_table[LNEG]      = &&L_LNEG;
        dispatch_table[LSHL]      = &&L_LSHL;
        dispatch_table[LSHR]      = &&L_LSHR;
        dispatch_table[LUSHR]     = &&L_LUSHR;
        dispatch_table[LAND]      = &&L_LAND;
        dispatch_table[LOR]       = &&L_LOR;
        dispatch_table[LXOR]      = &&L_LXOR;
        dispatch_table[LCMP]      = &&L_LCMP;
        dispatch_table[FCMPL]     = &&L_FCMPL;
        dispatch_table[FCMPG]     = &&L_FCMPG;
        dispatch_table[DCMPL]     = &&L_DCMPL;
        dispatch_table[DCMPG]     = &&L_DCMPG;
        dispatch_table[I2L]       = &&L_I2L;
        dispatch_table[I2F]       = &&L_I2F;
        dispatch_table[I2D]       = &&L_I2D;
        dispatch_table[L2I]       = &&L_L2I;
        dispatch_table[L2F]       = &&L_L2F;
        dispatch_table[L2D]       = &&L_L2D;
        dispatch_table[F2I]       = &&L_F2I;
        dispatch_table[F2L]       = &&L_F2L;
        dispatch_table[F2D]       = &&L_F2D;
        dispatch_table[D2I]       = &&L_D2I;
        dispatch_table[D2L]       = &&L_D2L;
        dispatch_table[D2F]       = &&L_D2F;
        dispatch_table[I2B]       = &&L_I2B;
        dispatch_table[I2C]       = &&L_I2C;
        dispatch_table[I2S]       = &&L_I2S;
        dispatch_table[IINC]      = &&L_IINC;
        dispatch_table[IFEQ]      = &&L_IFEQ;
        dispatch_table[IFNE]      = &&L_IFNE;
        dispatch_table[IFLT]      = &&L_IFLT;
        dispatch_table[IFGE]      = &&L_IFGE;
        dispatch_table[IFGT]      = &&L_IFGT;
        dispatch_table[IFLE]      = &&L_IFLE;
        dispatch_table[IFNULL]    = &&L_IFNULL;
        dispatch_table[IFNONNULL] = &&L_IFNONNULL;
        dispatch_table[IF_ICMPEQ] = &&L_IF_ICMPEQ;
        dispatch_table[IF_ICMPNE] = &&L_IF_ICMPNE;
        dispatch_table[IF_ICMPLT] = &&L_IF_ICMPLT;
        dispatch_table[IF_ICMPGE] = &&L_IF_ICMPGE;
        dispatch_table[IF_ICMPGT] = &&L_IF_ICMPGT;
        dispatch_table[IF_ICMPLE] = &&L_IF_ICMPLE;
        dispatch_table[IF_ACMPEQ] = &&L_IF_ACMPEQ;
        dispatch_table[IF_ACMPNE] = &&L_IF_ACMPNE;
        dispatch_table[GOTO]      = &&L_GOTO;
        dispatch_table[TABLESWITCH]  = &&L_TABLESWITCH;
        dispatch_table[LOOKUPSWITCH] = &&L_LOOKUPSWITCH;
        dispatch_table[RETURN]    = &&L_RETURN;
        dispatch_table[IRETURN]   = &&L_RETURN;
        dispatch_table[LRETURN]   = &&L_RETURN;
        dispatch_table[FRETURN]   = &&L_RETURN;
        dispatch_table[DRETURN]   = &&L_RETURN;
        dispatch_table[ARETURN]   = &&L_RETURN;
        dispatch_table[GETSTATIC] = &&L_GETSTATIC;
        dispatch_table[PUTSTATIC] = &&L_PUTSTATIC;
        dispatch_table[GETFIELD]  = &&L_GETFIELD;
        dispatch_table[PUTFIELD]  = &&L_PUTFIELD;
        dispatch_table[INVOKESTATIC]    = &&L_INVOKESTATIC;
        dispatch_table[INVOKEVIRTUAL]   = &&L_INVOKEVIRTUAL;
        dispatch_table[INVOKEINTERFACE] = &&L_INVOKEVIRTUAL;
        dispatch_table[INVOKESPECIAL]   = &&L_INVOKESPECIAL;
        dispatch_table[NEW]       = &&L_NEW;
        dispatch_table[NEWARRAY]  = &&L_NEWARRAY;
        dispatch_table[ANEWARRAY] = &&L_ANEWARRAY;
        dispatch_table[MULTIANEWARRAY] = &&L_MULTIANEWARRAY;
        dispatch_table[ARRAYLENGTH]    = &&L_ARRAYLENGTH;
        dispatch_table[CHECKCAST] = &&L_CHECKCAST;
        dispatch_table[INSTANCEOF] = &&L_INSTANCEOF;
        dispatch_table[ATHROW]    = &&L_ATHROW;
        dispatch_table[MONITORENTER] = &&L_MONITORENTER;
        dispatch_table[MONITOREXIT]  = &&L_MONITOREXIT;
        dispatch_table[WIDE]      = &&L_WIDE;
        dispatch_init = true;
    }

    uint8_t op = 0;

#define TRACE_OP() do { \
    if (__builtin_expect(g_trace, 0)) { \
        fprintf(stderr, "  [%-30s %-20s] pc=%4u sp=%2u op=0x%02x\n", \
            f.klass  ? f.klass->name.c_str()  : "?", \
            f.method ? f.method->name.c_str() : "?", \
            f.pc, f.sp, code[f.pc]); \
    } \
    if (__builtin_expect(g_heartbeat_insns > 0, 0)) { \
        if (++g_heartbeat_counter >= g_heartbeat_insns) { \
            g_heartbeat_counter = 0; \
            fprintf(stderr, "[hb t=%d] %s.%s pc=%u\n", \
                current_thread_ref(), \
                f.klass  ? f.klass->name.c_str()  : "?", \
                f.method ? f.method->name.c_str() : "?", \
                f.pc); \
        } \
    } \
} while (0)
#define DISPATCH() do { TRACE_OP(); op = code[f.pc++]; goto *dispatch_table[op]; } while (0)

    // Outer while+try so exception handlers can `continue` to re-enter dispatch
    // at an updated pc.
    while (true) {
    try {
        DISPATCH();

        // ── Constants ─────────────────────────────────────────────────────────
        L_NOP:          DISPATCH();
        L_ACONST_NULL:  f.push_ref(NULL_REF); DISPATCH();
        L_ICONST_M1:    f.push_int(-1); DISPATCH();
        L_ICONST_0:     f.push_int(0);  DISPATCH();
        L_ICONST_1:     f.push_int(1);  DISPATCH();
        L_ICONST_2:     f.push_int(2);  DISPATCH();
        L_ICONST_3:     f.push_int(3);  DISPATCH();
        L_ICONST_4:     f.push_int(4);  DISPATCH();
        L_ICONST_5:     f.push_int(5);  DISPATCH();
        L_LCONST_0:     f.push_long(0); DISPATCH();
        L_LCONST_1:     f.push_long(1); DISPATCH();
        L_FCONST_0:     f.push_float(0.0f); DISPATCH();
        L_FCONST_1:     f.push_float(1.0f); DISPATCH();
        L_FCONST_2:     f.push_float(2.0f); DISPATCH();
        L_DCONST_0:     f.push_double(0.0); DISPATCH();
        L_DCONST_1:     f.push_double(1.0); DISPATCH();

        L_BIPUSH:  f.push_int(bc_s1(code, f.pc)); f.pc += 1; DISPATCH();
        L_SIPUSH:  f.push_int(bc_s2(code, f.pc)); f.pc += 2; DISPATCH();

        L_LDC:
            exec_ldc(*this, f, *cf_ptr, bc_u1(code, f.pc));
            f.pc += 1; DISPATCH();
        L_LDC_W:
            exec_ldc(*this, f, *cf_ptr, bc_u2(code, f.pc));
            f.pc += 2; DISPATCH();

        // ── Loads ─────────────────────────────────────────────────────────────
        L_ILOAD: f.push(f.locals[bc_u1(code,f.pc)]); f.pc+=1; DISPATCH();
        L_LLOAD: {
            uint8_t idx = bc_u1(code,f.pc); f.pc+=1;
            f.push_long(f.get_long(idx)); DISPATCH();
        }
        L_ALOAD: f.push(f.locals[bc_u1(code,f.pc)]); f.pc+=1; DISPATCH();

        L_ILOAD_0: f.push(f.locals[0]); DISPATCH();
        L_ILOAD_1: f.push(f.locals[1]); DISPATCH();
        L_ILOAD_2: f.push(f.locals[2]); DISPATCH();
        L_ILOAD_3: f.push(f.locals[3]); DISPATCH();

        L_LLOAD_0: f.push_long(f.get_long(0)); DISPATCH();
        L_LLOAD_1: f.push_long(f.get_long(1)); DISPATCH();
        L_LLOAD_2: f.push_long(f.get_long(2)); DISPATCH();
        L_LLOAD_3: f.push_long(f.get_long(3)); DISPATCH();

        L_ALOAD_0: f.push(f.locals[0]); DISPATCH();
        L_ALOAD_1: f.push(f.locals[1]); DISPATCH();
        L_ALOAD_2: f.push(f.locals[2]); DISPATCH();
        L_ALOAD_3: f.push(f.locals[3]); DISPATCH();

        // ── Array loads ───────────────────────────────────────────────────────
        L_IALOAD: {
            int32_t idx = f.pop_int();
            ObjRef  arr = f.pop_ref();
            auto* obj = m_heap.deref(arr);
            if (!obj) throw JvmException{NULL_REF, "NullPointerException"};
            f.push(obj->array_slots()[idx]);
            DISPATCH();
        }
        L_LALOAD: {
            int32_t idx = f.pop_int();
            ObjRef  arr = f.pop_ref();
            auto* obj = m_heap.deref(arr);
            if (!obj) throw JvmException{NULL_REF, "NullPointerException"};
            f.push_long(obj->array_longs()[idx]);
            DISPATCH();
        }
        L_AALOAD: {
            int32_t idx = f.pop_int();
            ObjRef  arr = f.pop_ref();
            auto* obj = m_heap.deref(arr);
            if (!obj) throw JvmException{NULL_REF, "NullPointerException"};
            f.push_ref(obj->array_slots()[idx].as_ref());
            DISPATCH();
        }
        L_BALOAD: {
            int32_t idx = f.pop_int();
            ObjRef  arr = f.pop_ref();
            auto* obj = m_heap.deref(arr);
            if (!obj) throw JvmException{NULL_REF, "NullPointerException"};
            f.push_int(static_cast<int8_t>(obj->array_bytes()[idx]));
            DISPATCH();
        }
        L_CALOAD: {
            int32_t idx = f.pop_int();
            ObjRef  arr = f.pop_ref();
            auto* obj = m_heap.deref(arr);
            if (!obj) throw JvmException{NULL_REF, "NullPointerException"};
            f.push_int(obj->array_shorts()[idx]);
            DISPATCH();
        }
        L_SALOAD: {
            int32_t idx = f.pop_int();
            ObjRef  arr = f.pop_ref();
            auto* obj = m_heap.deref(arr);
            if (!obj) throw JvmException{NULL_REF, "NullPointerException"};
            f.push_int(static_cast<int16_t>(obj->array_shorts()[idx]));
            DISPATCH();
        }

        // ── Stores ────────────────────────────────────────────────────────────
        L_ISTORE: f.locals[bc_u1(code,f.pc)] = f.pop(); f.pc+=1; DISPATCH();
        L_LSTORE: {
            uint8_t idx = bc_u1(code,f.pc); f.pc+=1;
            f.set_long(idx, f.pop_long()); DISPATCH();
        }
        L_ASTORE: f.locals[bc_u1(code,f.pc)] = f.pop(); f.pc+=1; DISPATCH();

        L_ISTORE_0: f.locals[0] = f.pop(); DISPATCH();
        L_ISTORE_1: f.locals[1] = f.pop(); DISPATCH();
        L_ISTORE_2: f.locals[2] = f.pop(); DISPATCH();
        L_ISTORE_3: f.locals[3] = f.pop(); DISPATCH();

        L_LSTORE_0: f.set_long(0, f.pop_long()); DISPATCH();
        L_LSTORE_1: f.set_long(1, f.pop_long()); DISPATCH();
        L_LSTORE_2: f.set_long(2, f.pop_long()); DISPATCH();
        L_LSTORE_3: f.set_long(3, f.pop_long()); DISPATCH();

        L_ASTORE_0: f.locals[0] = f.pop(); DISPATCH();
        L_ASTORE_1: f.locals[1] = f.pop(); DISPATCH();
        L_ASTORE_2: f.locals[2] = f.pop(); DISPATCH();
        L_ASTORE_3: f.locals[3] = f.pop(); DISPATCH();

        // ── Array stores ──────────────────────────────────────────────────────
        L_IASTORE: {
            Slot    val = f.pop();
            int32_t idx = f.pop_int();
            ObjRef  arr = f.pop_ref();
            auto* obj = m_heap.deref(arr);
            if (!obj) throw JvmException{NULL_REF, "NullPointerException"};
            obj->array_slots()[idx] = val;
            DISPATCH();
        }
        L_LASTORE: {
            int64_t val = f.pop_long();
            int32_t idx = f.pop_int();
            ObjRef  arr = f.pop_ref();
            auto* obj = m_heap.deref(arr);
            if (!obj) throw JvmException{NULL_REF, "NullPointerException"};
            obj->array_longs()[idx] = val;
            DISPATCH();
        }
        L_AASTORE: {
            ObjRef  val = f.pop_ref();
            int32_t idx = f.pop_int();
            ObjRef  arr = f.pop_ref();
            auto* obj = m_heap.deref(arr);
            if (!obj) throw JvmException{NULL_REF, "NullPointerException"};
            obj->array_slots()[idx] = Slot::from_ref(val);
            DISPATCH();
        }
        L_BASTORE: {
            int32_t val = f.pop_int();
            int32_t idx = f.pop_int();
            ObjRef  arr = f.pop_ref();
            auto* obj = m_heap.deref(arr);
            if (!obj) throw JvmException{NULL_REF, "NullPointerException"};
            obj->array_bytes()[idx] = static_cast<uint8_t>(val);
            DISPATCH();
        }
        L_CASTORE: {
            int32_t val = f.pop_int();
            int32_t idx = f.pop_int();
            ObjRef  arr = f.pop_ref();
            auto* obj = m_heap.deref(arr);
            if (!obj) throw JvmException{NULL_REF, "NullPointerException"};
            obj->array_shorts()[idx] = static_cast<uint16_t>(val);
            DISPATCH();
        }
        L_SASTORE: {
            int32_t val = f.pop_int();
            int32_t idx = f.pop_int();
            ObjRef  arr = f.pop_ref();
            auto* obj = m_heap.deref(arr);
            if (!obj) throw JvmException{NULL_REF, "NullPointerException"};
            obj->array_shorts()[idx] = static_cast<uint16_t>(val);
            DISPATCH();
        }

        // ── Stack manipulation ────────────────────────────────────────────────
        L_POP:   f.pop(); DISPATCH();
        L_POP2:  f.pop(); f.pop(); DISPATCH();
        L_DUP:   f.push(f.peek()); DISPATCH();
        L_DUP_X1: {
            Slot v1 = f.pop(), v2 = f.pop();
            f.push(v1); f.push(v2); f.push(v1);
            DISPATCH();
        }
        L_DUP_X2: {
            // Form 1: ..., v3, v2, v1 → ..., v1, v3, v2, v1
            Slot v1 = f.pop(), v2 = f.pop(), v3 = f.pop();
            f.push(v1); f.push(v3); f.push(v2); f.push(v1);
            DISPATCH();
        }
        L_DUP2: {
            Slot v1 = f.peek(0), v2 = f.peek(1);
            f.push(v2); f.push(v1);
            DISPATCH();
        }
        L_DUP2_X1: {
            // ..., v3, v2, v1 → ..., v2, v1, v3, v2, v1
            Slot v1 = f.pop(), v2 = f.pop(), v3 = f.pop();
            f.push(v2); f.push(v1); f.push(v3); f.push(v2); f.push(v1);
            DISPATCH();
        }
        L_DUP2_X2: {
            // ..., v4, v3, v2, v1 → ..., v2, v1, v4, v3, v2, v1
            Slot v1 = f.pop(), v2 = f.pop(), v3 = f.pop(), v4 = f.pop();
            f.push(v2); f.push(v1); f.push(v4); f.push(v3); f.push(v2); f.push(v1);
            DISPATCH();
        }
        L_SWAP: {
            Slot v1 = f.pop(), v2 = f.pop();
            f.push(v1); f.push(v2);
            DISPATCH();
        }

        // ── Integer arithmetic ────────────────────────────────────────────────
        // Java requires two's-complement wrap on overflow, which is UB on
        // signed C++ ints. Route through unsigned (defined to wrap) and cast
        // back so -O2/UBSan don't miscompile or trap.
        L_IADD: { uint32_t b=static_cast<uint32_t>(f.pop_int()), a=static_cast<uint32_t>(f.pop_int()); f.push_int(static_cast<int32_t>(a+b)); DISPATCH(); }
        L_ISUB: { uint32_t b=static_cast<uint32_t>(f.pop_int()), a=static_cast<uint32_t>(f.pop_int()); f.push_int(static_cast<int32_t>(a-b)); DISPATCH(); }
        L_IMUL: { uint32_t b=static_cast<uint32_t>(f.pop_int()), a=static_cast<uint32_t>(f.pop_int()); f.push_int(static_cast<int32_t>(a*b)); DISPATCH(); }
        L_IDIV: {
            int32_t b=f.pop_int(), a=f.pop_int();
            if (b==0) throw JvmException{NULL_REF,"ArithmeticException: / by zero"};
            // INT_MIN / -1 is UB in C++; Java defines it to wrap to INT_MIN.
            if (a==INT32_MIN && b==-1) { f.push_int(INT32_MIN); DISPATCH(); }
            f.push_int(a/b); DISPATCH();
        }
        L_IREM: {
            int32_t b=f.pop_int(), a=f.pop_int();
            if (b==0) throw JvmException{NULL_REF,"ArithmeticException: / by zero"};
            if (a==INT32_MIN && b==-1) { f.push_int(0); DISPATCH(); }
            f.push_int(a%b); DISPATCH();
        }
        L_INEG: { uint32_t v=static_cast<uint32_t>(f.pop_int()); f.push_int(static_cast<int32_t>(0u - v)); DISPATCH(); }
        L_ISHL: { int32_t s=f.pop_int()&0x1f; uint32_t v=static_cast<uint32_t>(f.pop_int()); f.push_int(static_cast<int32_t>(v<<s)); DISPATCH(); }
        L_ISHR: { int32_t s=f.pop_int()&0x1f, v=f.pop_int(); f.push_int(v>>s); DISPATCH(); }
        L_IUSHR:{ int32_t s=f.pop_int()&0x1f; uint32_t v=static_cast<uint32_t>(f.pop_int()); f.push_int(static_cast<int32_t>(v>>s)); DISPATCH(); }
        L_IAND: { int32_t b=f.pop_int(), a=f.pop_int(); f.push_int(a&b); DISPATCH(); }
        L_IOR:  { int32_t b=f.pop_int(), a=f.pop_int(); f.push_int(a|b); DISPATCH(); }
        L_IXOR: { int32_t b=f.pop_int(), a=f.pop_int(); f.push_int(a^b); DISPATCH(); }

        // ── Float arithmetic ──────────────────────────────────────────────────
        L_FADD: { float  b=f.pop_float(),  a=f.pop_float();  f.push_float(a+b);  DISPATCH(); }
        L_FSUB: { float  b=f.pop_float(),  a=f.pop_float();  f.push_float(a-b);  DISPATCH(); }
        L_FMUL: { float  b=f.pop_float(),  a=f.pop_float();  f.push_float(a*b);  DISPATCH(); }
        L_FDIV: { float  b=f.pop_float(),  a=f.pop_float();  f.push_float(a/b);  DISPATCH(); }
        L_FREM: { float  b=f.pop_float(),  a=f.pop_float();  f.push_float(std::fmod(a,b)); DISPATCH(); }
        L_FNEG: { f.push_float(-f.pop_float()); DISPATCH(); }

        // ── Double arithmetic ─────────────────────────────────────────────────
        L_DADD: { double b=f.pop_double(), a=f.pop_double(); f.push_double(a+b); DISPATCH(); }
        L_DSUB: { double b=f.pop_double(), a=f.pop_double(); f.push_double(a-b); DISPATCH(); }
        L_DMUL: { double b=f.pop_double(), a=f.pop_double(); f.push_double(a*b); DISPATCH(); }
        L_DDIV: { double b=f.pop_double(), a=f.pop_double(); f.push_double(a/b); DISPATCH(); }
        L_DREM: { double b=f.pop_double(), a=f.pop_double(); f.push_double(std::fmod(a,b)); DISPATCH(); }
        L_DNEG: { f.push_double(-f.pop_double()); DISPATCH(); }

        // ── Long arithmetic ───────────────────────────────────────────────────
        L_LADD: { uint64_t b=static_cast<uint64_t>(f.pop_long()), a=static_cast<uint64_t>(f.pop_long()); f.push_long(static_cast<int64_t>(a+b)); DISPATCH(); }
        L_LSUB: { uint64_t b=static_cast<uint64_t>(f.pop_long()), a=static_cast<uint64_t>(f.pop_long()); f.push_long(static_cast<int64_t>(a-b)); DISPATCH(); }
        L_LMUL: { uint64_t b=static_cast<uint64_t>(f.pop_long()), a=static_cast<uint64_t>(f.pop_long()); f.push_long(static_cast<int64_t>(a*b)); DISPATCH(); }
        L_LDIV: { int64_t b=f.pop_long(), a=f.pop_long();
                     if (b==0) throw JvmException{NULL_REF,"ArithmeticException: / by zero"};
                     if (a==INT64_MIN && b==-1) { f.push_long(INT64_MIN); DISPATCH(); }
                     f.push_long(a/b); DISPATCH(); }
        L_LREM: { int64_t b=f.pop_long(), a=f.pop_long();
                     if (b==0) throw JvmException{NULL_REF,"ArithmeticException: / by zero"};
                     if (a==INT64_MIN && b==-1) { f.push_long(0); DISPATCH(); }
                     f.push_long(a%b); DISPATCH(); }
        L_LNEG: { uint64_t v=static_cast<uint64_t>(f.pop_long()); f.push_long(static_cast<int64_t>(0ull - v)); DISPATCH(); }
        L_LSHL: { int32_t s=f.pop_int()&0x3f; uint64_t v=static_cast<uint64_t>(f.pop_long()); f.push_long(static_cast<int64_t>(v<<s)); DISPATCH(); }
        L_LSHR: { int32_t s=f.pop_int()&0x3f; int64_t v=f.pop_long(); f.push_long(v>>s); DISPATCH(); }
        L_LUSHR:{ int32_t s=f.pop_int()&0x3f; uint64_t v=static_cast<uint64_t>(f.pop_long()); f.push_long(static_cast<int64_t>(v>>s)); DISPATCH(); }
        L_LAND: { int64_t b=f.pop_long(), a=f.pop_long(); f.push_long(a&b); DISPATCH(); }
        L_LOR:  { int64_t b=f.pop_long(), a=f.pop_long(); f.push_long(a|b); DISPATCH(); }
        L_LXOR: { int64_t b=f.pop_long(), a=f.pop_long(); f.push_long(a^b); DISPATCH(); }
        L_LCMP: {
            int64_t b=f.pop_long(), a=f.pop_long();
            f.push_int(a>b ? 1 : a<b ? -1 : 0); DISPATCH();
        }
        // Float/double compare: L variants push -1 if NaN, G push +1.
        L_FCMPL: {
            float b=f.pop_float(), a=f.pop_float();
            f.push_int((std::isnan(a)||std::isnan(b)) ? -1 : a>b ? 1 : a<b ? -1 : 0);
            DISPATCH();
        }
        L_FCMPG: {
            float b=f.pop_float(), a=f.pop_float();
            f.push_int((std::isnan(a)||std::isnan(b)) ? 1 : a>b ? 1 : a<b ? -1 : 0);
            DISPATCH();
        }
        L_DCMPL: {
            double b=f.pop_double(), a=f.pop_double();
            f.push_int((std::isnan(a)||std::isnan(b)) ? -1 : a>b ? 1 : a<b ? -1 : 0);
            DISPATCH();
        }
        L_DCMPG: {
            double b=f.pop_double(), a=f.pop_double();
            f.push_int((std::isnan(a)||std::isnan(b)) ? 1 : a>b ? 1 : a<b ? -1 : 0);
            DISPATCH();
        }

        // ── Conversions ───────────────────────────────────────────────────────
        L_I2L:  f.push_long  (f.pop_int());                                 DISPATCH();
        L_I2F:  f.push_float (static_cast<float> (f.pop_int()));            DISPATCH();
        L_I2D:  f.push_double(static_cast<double>(f.pop_int()));            DISPATCH();
        L_L2I:  f.push_int   (static_cast<int32_t>(f.pop_long()));          DISPATCH();
        L_L2F:  f.push_float (static_cast<float>  (f.pop_long()));          DISPATCH();
        L_L2D:  f.push_double(static_cast<double> (f.pop_long()));          DISPATCH();
        L_F2I:  f.push_int   (static_cast<int32_t>(f.pop_float()));         DISPATCH();
        L_F2L:  f.push_long  (static_cast<int64_t>(f.pop_float()));         DISPATCH();
        L_F2D:  f.push_double(static_cast<double> (f.pop_float()));         DISPATCH();
        L_D2I:  f.push_int   (static_cast<int32_t>(f.pop_double()));        DISPATCH();
        L_D2L:  f.push_long  (static_cast<int64_t>(f.pop_double()));        DISPATCH();
        L_D2F:  f.push_float (static_cast<float>  (f.pop_double()));        DISPATCH();
        L_I2B:  f.push_int(static_cast<int32_t>(static_cast<int8_t> (f.pop_int()))); DISPATCH();
        L_I2C:  f.push_int(static_cast<int32_t>(static_cast<uint16_t>(f.pop_int()))); DISPATCH();
        L_I2S:  f.push_int(static_cast<int32_t>(static_cast<int16_t> (f.pop_int()))); DISPATCH();

        // ── IINC ─────────────────────────────────────────────────────────────
        L_IINC: {
            uint8_t idx = bc_u1(code, f.pc);
            int8_t  c   = bc_s1(code, f.pc+1);
            f.locals[idx].raw += c;
            f.pc += 2; DISPATCH();
        }

        // ── Branches ──────────────────────────────────────────────────────────
        // Branch offsets are relative to the opcode's own address (f.pc-1).
        // Save base = opcode address, then set f.pc = base + off if taken.
        #define BRANCH(cond) { uint32_t base=f.pc-1; int16_t off=bc_s2(code,f.pc); f.pc+=2; if(cond) f.pc=static_cast<uint32_t>(base+off); DISPATCH(); }
        L_IFEQ:      { int32_t v=f.pop_int(); BRANCH(v==0) }
        L_IFNE:      { int32_t v=f.pop_int(); BRANCH(v!=0) }
        L_IFLT:      { int32_t v=f.pop_int(); BRANCH(v< 0) }
        L_IFGE:      { int32_t v=f.pop_int(); BRANCH(v>=0) }
        L_IFGT:      { int32_t v=f.pop_int(); BRANCH(v> 0) }
        L_IFLE:      { int32_t v=f.pop_int(); BRANCH(v<=0) }
        L_IFNULL:    { ObjRef r=f.pop_ref(); BRANCH(r==NULL_REF) }
        L_IFNONNULL: { ObjRef r=f.pop_ref(); BRANCH(r!=NULL_REF) }
        L_IF_ICMPEQ: { int32_t b=f.pop_int(),a=f.pop_int(); BRANCH(a==b) }
        L_IF_ICMPNE: { int32_t b=f.pop_int(),a=f.pop_int(); BRANCH(a!=b) }
        L_IF_ICMPLT: { int32_t b=f.pop_int(),a=f.pop_int(); BRANCH(a< b) }
        L_IF_ICMPGE: { int32_t b=f.pop_int(),a=f.pop_int(); BRANCH(a>=b) }
        L_IF_ICMPGT: { int32_t b=f.pop_int(),a=f.pop_int(); BRANCH(a> b) }
        L_IF_ICMPLE: { int32_t b=f.pop_int(),a=f.pop_int(); BRANCH(a<=b) }
        L_IF_ACMPEQ: { ObjRef b=f.pop_ref(),a=f.pop_ref(); BRANCH(a==b) }
        L_IF_ACMPNE: { ObjRef b=f.pop_ref(),a=f.pop_ref(); BRANCH(a!=b) }
        #undef BRANCH

        L_GOTO: {
            int16_t off = bc_s2(code, f.pc);
            f.pc += off - 1;  // -1 because we already incremented past opcode
            DISPATCH();
        }

        // ── tableswitch ───────────────────────────────────────────────────────
        L_TABLESWITCH: {
            uint32_t base_pc = f.pc - 1;
            // Align to 4-byte boundary after the opcode
            uint32_t pad = (4 - (f.pc % 4)) % 4;
            f.pc += pad;
            int32_t def    = bc_s4(code, f.pc); f.pc += 4;
            int32_t lo     = bc_s4(code, f.pc); f.pc += 4;
            int32_t hi     = bc_s4(code, f.pc); f.pc += 4;
            int32_t key    = f.pop_int();
            int32_t offset = def;
            if (key >= lo && key <= hi) {
                uint32_t entry = static_cast<uint32_t>(key - lo);
                offset = bc_s4(code, f.pc + entry * 4);
            }
            f.pc = static_cast<uint32_t>(static_cast<int32_t>(base_pc) + offset);
            DISPATCH();
        }

        // ── lookupswitch ──────────────────────────────────────────────────────
        L_LOOKUPSWITCH: {
            uint32_t base_pc = f.pc - 1;
            uint32_t pad = (4 - (f.pc % 4)) % 4;
            f.pc += pad;
            int32_t def   = bc_s4(code, f.pc); f.pc += 4;
            int32_t npairs= bc_s4(code, f.pc); f.pc += 4;
            int32_t key   = f.pop_int();
            int32_t offset= def;
            for (int32_t i = 0; i < npairs; ++i) {
                int32_t match = bc_s4(code, f.pc);
                int32_t off   = bc_s4(code, f.pc+4);
                f.pc += 8;
                if (key == match) { offset = off; break; }
            }
            f.pc = static_cast<uint32_t>(static_cast<int32_t>(base_pc) + offset);
            DISPATCH();
        }

        // ── Returns ───────────────────────────────────────────────────────────
        L_RETURN: return;

        // ── Static fields ─────────────────────────────────────────────────────
        L_GETSTATIC: {
            auto [klass, fd] = resolve_field(*cf_ptr, bc_u2(code, f.pc));
            f.pc += 2;
            initialize_class(klass);
            f.push(klass->static_slot(fd->slot_index));
            if (fd->is_long() || fd->is_double())
                f.push(klass->static_slot(fd->slot_index + 1));
            DISPATCH();
        }
        L_PUTSTATIC: {
            auto [klass, fd] = resolve_field(*cf_ptr, bc_u2(code, f.pc));
            f.pc += 2;
            initialize_class(klass);
            if (fd->is_long() || fd->is_double()) {
                klass->static_slot(fd->slot_index + 1) = f.pop();
                klass->static_slot(fd->slot_index    ) = f.pop();
            } else {
                Slot val = f.pop();
                klass->static_slot(fd->slot_index) = val;
            }
            DISPATCH();
        }

        // ── Instance fields ───────────────────────────────────────────────────
        L_GETFIELD: {
            auto [klass, fd] = resolve_field(*cf_ptr, bc_u2(code, f.pc));
            f.pc += 2;
            ObjRef ref = f.pop_ref();
            auto* obj = m_heap.deref(ref);
            if (!obj) throw JvmException{NULL_REF, "NullPointerException"};
            f.push(obj->field(fd->slot_index));
            if (fd->is_long() || fd->is_double())
                f.push(obj->field(fd->slot_index + 1));
            DISPATCH();
        }
        L_PUTFIELD: {
            auto [klass, fd] = resolve_field(*cf_ptr, bc_u2(code, f.pc));
            f.pc += 2;
            if (fd->is_long() || fd->is_double()) {
                Slot hi = f.pop(), lo = f.pop();
                ObjRef ref = f.pop_ref();
                auto* obj = m_heap.deref(ref);
                if (!obj) throw JvmException{NULL_REF, "NullPointerException"};
                obj->field(fd->slot_index    ) = lo;
                obj->field(fd->slot_index + 1) = hi;
            } else {
                Slot val = f.pop();
                ObjRef ref = f.pop_ref();
                auto* obj = m_heap.deref(ref);
                if (!obj) throw JvmException{NULL_REF, "NullPointerException"};
                obj->field(fd->slot_index) = val;
            }
            DISPATCH();
        }

        // ── Invocations ───────────────────────────────────────────────────────
        L_INVOKESTATIC: {
            auto [klass, md] = resolve_method(*cf_ptr, bc_u2(code, f.pc));
            f.pc += 2;
            initialize_class(klass);
            uint32_t nslots = md->arg_slot_count;
            do_invoke(*this, f, md, klass, nslots);
            DISPATCH();
        }
        L_INVOKEVIRTUAL: {
            uint16_t cp_idx = bc_u2(code, f.pc);
            auto [klass, md] = resolve_method(*cf_ptr, cp_idx);
            uint32_t call_site_pc = f.pc - 1;  // pc was already past the opcode
            f.pc += 2;
            if (op == INVOKEINTERFACE) f.pc += 2;  // count + 0 bytes
            // +1 for 'this'
            uint32_t nslots = md->arg_slot_count + 1;
            if (f.sp < nslots) {
                throw std::runtime_error(
                    "Stack underflow in invokevirtual " +
                    klass->name + "." + md->name + md->descriptor +
                    " (sp=" + std::to_string(f.sp) +
                    ", need=" + std::to_string(nslots) + ")");
            }
            // Peek at 'this' to get actual runtime type for virtual dispatch
            ObjRef this_ref = f.peek(nslots - 1).as_ref();
            if (this_ref == NULL_REF)
                throw JvmException{NULL_REF, "NullPointerException"};
            auto* obj = m_heap.deref(this_ref);
            ClassDef* actual = obj ? obj->klass : klass;
            // XOR the PC (rotated) with the method pointer so every call
            // site gets a unique key. A left shift would discard the low
            // bits of the pointer, which collide across MethodDefs in the
            // same std::vector.
            uint64_t ic_key = reinterpret_cast<uintptr_t>(f.method) ^
                              (static_cast<uint64_t>(call_site_pc) * 0x9E3779B97F4A7C15ULL);
            MethodDef* vmd = vi_lookup(ic_key, actual, md);
            do_invoke(*this, f, vmd, actual ? actual : klass, nslots);
            DISPATCH();
        }
        L_INVOKESPECIAL: {
            auto [klass, md] = resolve_method(*cf_ptr, bc_u2(code, f.pc));
            f.pc += 2;
            uint32_t nslots = md->arg_slot_count + 1;
            do_invoke(*this, f, md, klass, nslots);
            DISPATCH();
        }

        // ── Object creation ───────────────────────────────────────────────────
        L_NEW: {
            ClassDef* klass = resolve_class(*cf_ptr, bc_u2(code, f.pc));
            f.pc += 2;
            f.push_ref(new_object(klass));
            DISPATCH();
        }
        L_NEWARRAY: {
            uint8_t  atype  = bc_u1(code, f.pc); f.pc += 1;
            int32_t  length = f.pop_int();
            ArrayType at    = static_cast<ArrayType>(atype);
            const char* arr_name;
            switch (at) {
                case ArrayType::Boolean: arr_name = "[Z"; break;
                case ArrayType::Char:    arr_name = "[C"; break;
                case ArrayType::Float:   arr_name = "[F"; break;
                case ArrayType::Double:  arr_name = "[D"; break;
                case ArrayType::Byte:    arr_name = "[B"; break;
                case ArrayType::Short:   arr_name = "[S"; break;
                case ArrayType::Int:     arr_name = "[I"; break;
                case ArrayType::Long:    arr_name = "[J"; break;
                default:                 arr_name = "[B"; break;
            }
            ObjRef arr;
            if (at == ArrayType::Long || at == ArrayType::Double)
                arr = m_heap.alloc_long_array(length,
                          m_loader.find_or_stub(arr_name));
            else
                arr = m_heap.alloc_prim_array(at, length,
                          m_loader.find_or_stub(arr_name));
            if (arr == NULL_REF) throw std::runtime_error("OutOfMemoryError");
            f.push_ref(arr);
            DISPATCH();
        }
        L_ANEWARRAY: {
            ClassDef* elem = resolve_class(*cf_ptr, bc_u2(code, f.pc));
            f.pc += 2;
            int32_t length = f.pop_int();
            ObjRef arr = m_heap.alloc_ref_array(length,
                             m_loader.find_or_stub("[L" + elem->name + ";"));
            if (arr == NULL_REF) throw std::runtime_error("OutOfMemoryError");
            f.push_ref(arr);
            DISPATCH();
        }
        L_MULTIANEWARRAY: {
            uint16_t cp_idx = bc_u2(code, f.pc); f.pc += 2;
            uint8_t  dims   = bc_u1(code, f.pc); f.pc += 1;
            ClassDef* klass = resolve_class(*cf_ptr, cp_idx);
            // Only handle 2D int arrays (the common case: int[][]).
            // dim_sizes must be destroyed before DISPATCH() — clang disallows
            // indirect goto across a scope with non-trivial destructors.
            ObjRef outer;
            {
                std::vector<int32_t> dim_sizes(dims);
                for (int i = dims-1; i >= 0; --i)
                    dim_sizes[i] = f.pop_int();
                outer = m_heap.alloc_ref_array(dim_sizes[0], klass);
                if (outer == NULL_REF) throw std::runtime_error("OutOfMemoryError");
                if (dims >= 2) {
                    auto* outer_obj = m_heap.deref(outer);
                    for (int32_t i = 0; i < dim_sizes[0]; ++i) {
                        ObjRef inner = m_heap.alloc_prim_array(
                            ArrayType::Int, dim_sizes[1],
                            m_loader.find_or_stub("[I"));
                        outer_obj->array_slots()[i] = Slot::from_ref(inner);
                    }
                }
            }
            f.push_ref(outer);
            DISPATCH();
        }
        L_ARRAYLENGTH: {
            ObjRef arr = f.pop_ref();
            auto* obj = m_heap.deref(arr);
            if (!obj) throw JvmException{NULL_REF, "NullPointerException"};
            f.push_int(obj->array_length());
            DISPATCH();
        }

        // ── Type checks ───────────────────────────────────────────────────────
        L_CHECKCAST: {
            f.pc += 2;  // skip CP index; no-op for now (trust the game)
            DISPATCH();
        }
        L_INSTANCEOF: {
            ClassDef* target = resolve_class(*cf_ptr, bc_u2(code, f.pc));
            f.pc += 2;
            ObjRef ref = f.pop_ref();
            if (ref == NULL_REF) { f.push_int(0); DISPATCH(); }
            auto* obj = m_heap.deref(ref);
            bool result = obj && obj->klass && obj->klass->is_subclass_of(target);
            f.push_int(result ? 1 : 0);
            DISPATCH();
        }

        // ── Exception ─────────────────────────────────────────────────────────
        L_ATHROW: {
            ObjRef ex = f.pop_ref();
            throw JvmException{ex, "Java exception thrown"};
        }

        // ── Monitors (no-op; J2ME games are mostly single-threaded in logic) ──
        L_MONITORENTER: f.pop_ref(); DISPATCH();
        L_MONITOREXIT:  f.pop_ref(); DISPATCH();

        // ── WIDE prefix ───────────────────────────────────────────────────────
        L_WIDE: {
            uint8_t wide_op = bc_u1(code, f.pc++);
            uint16_t idx    = bc_u2(code, f.pc); f.pc += 2;
            switch (wide_op) {
                case ILOAD: case ALOAD: f.push(f.locals[idx]); break;
                case LLOAD: f.push_long(f.get_long(idx)); break;
                case ISTORE: case ASTORE: f.locals[idx] = f.pop(); break;
                case LSTORE: f.set_long(idx, f.pop_long()); break;
                case IINC: {
                    int16_t c = bc_s2(code, f.pc); f.pc += 2;
                    f.locals[idx].raw += c;
                    break;
                }
                default:
                    throw std::runtime_error("Unsupported wide opcode: " +
                                             std::to_string(wide_op));
            }
            DISPATCH();
        }

        L_default: {
            std::string msg = "Unimplemented opcode 0x" +
                              ([op]{ char b[4]; snprintf(b,4,"%02x",op); return std::string(b); }()) +
                              " at " + frame_loc();
            fprintf(stderr, "[vm] %s\n", msg.c_str());
            throw std::runtime_error(msg);
        }
    } catch (JvmException& e) {
        uint32_t throw_pc = f.pc > 0 ? f.pc - 1 : 0;
        if (e.location.empty() && cf_ptr)
            e.location = cf_ptr->this_class + "." +
                         (f.method ? f.method->name : "?") + "@" + std::to_string(throw_pc);
        if (getenv("J2ME_TRACE_EXC")) {
            fprintf(stderr, "[exc] %s at %s (pc=%u, frame %s.%s)\n",
                    e.message.c_str(), e.location.c_str(), f.pc,
                    cf_ptr ? cf_ptr->this_class.c_str() : "?",
                    f.method ? f.method->name.c_str() : "?");
        }
        for (const auto& entry : ex_tbl) {
            if (throw_pc < entry.start_pc || throw_pc >= entry.end_pc) continue;
            // catch_type == 0 means "catch all" (finally)
            bool matches = (entry.catch_type == 0);
            if (!matches && cf_ptr) {
                // Resolve the catch type class and check subtype
                const auto& cp_entry = cf_ptr->constant_pool.at(entry.catch_type);
                if (const auto* cls = std::get_if<CpClass>(&cp_entry)) {
                    ClassDef* catch_klass = m_loader.find_or_stub(cf_ptr->utf8(cls->name_index));
                    // JvmException has no real class hierarchy in our VM; match
                    // java/lang/Exception and java/lang/Throwable to all JvmExceptions,
                    // and also match if catch_klass name equals the exception message
                    // (best-effort for now — NullPointerException etc.)
                    const std::string& cn = catch_klass->name;
                    matches = (cn == "java/lang/Exception"   ||
                               cn == "java/lang/RuntimeException" ||
                               cn == "java/lang/Throwable"   ||
                               cn == "java/lang/Error"       ||
                               cn == "java/lang/Object");
                    // Also accept if the catch class name ends with the exception kind
                    if (!matches && !e.message.empty()) {
                        std::string simple = cn;
                        auto slash = simple.rfind('/');
                        if (slash != std::string::npos) simple = simple.substr(slash + 1);
                        matches = (simple == e.message);
                    }
                    // Hard-coded subtype relations: our synthetic JvmExceptions
                    // don't carry a real class hierarchy, so map common
                    // subclass relationships by hand. EOFException/FileNotFound
                    // etc. all extend IOException.
                    if (!matches && cn == "java/io/IOException") {
                        matches = (e.message == "EOFException" ||
                                   e.message == "FileNotFoundException" ||
                                   e.message == "UTFDataFormatException" ||
                                   e.message == "InterruptedIOException" ||
                                   e.message == "IOException");
                    }
                    if (!matches && cn == "java/lang/RuntimeException") {
                        matches = (e.message == "NullPointerException" ||
                                   e.message == "ArrayIndexOutOfBoundsException" ||
                                   e.message == "IndexOutOfBoundsException" ||
                                   e.message == "ArithmeticException" ||
                                   e.message == "ClassCastException" ||
                                   e.message == "NumberFormatException" ||
                                   e.message == "IllegalArgumentException" ||
                                   e.message == "IllegalStateException");
                    }
                    if (!matches && cn == "java/lang/IndexOutOfBoundsException") {
                        matches = (e.message == "ArrayIndexOutOfBoundsException" ||
                                   e.message == "StringIndexOutOfBoundsException" ||
                                   e.message == "IndexOutOfBoundsException");
                    }
                    if (!matches && cn == "javax/microedition/rms/RecordStoreException") {
                        matches = (e.message == "RecordStoreNotFoundException" ||
                                   e.message == "RecordStoreNotOpenException" ||
                                   e.message == "RecordStoreFullException" ||
                                   e.message == "InvalidRecordIDException" ||
                                   e.message == "RecordStoreException");
                    }
                }
            }
            if (!matches) continue;

            // Found a matching handler: clear the stack, push exception ref, jump.
            // Always use a valid heap object so that e.printStackTrace() etc. work.
            // e.ref may be NULL_REF (synthetic exception) or an out-of-range stale ref.
            ObjRef ex_ref = e.ref;
            if (!m_heap.valid(ex_ref)) {
                ClassDef* ex_klass = m_loader.find_or_stub("java/lang/Exception");
                ex_ref = m_heap.alloc_object(ex_klass, 0);
            }
            f.sp = 0;
            f.push_ref(ex_ref);
            f.pc = entry.handler_pc;
            goto dispatch_restart;   // re-enter the interpreter loop
        }
        // No handler in this frame: annotate and propagate
        if (e.location.empty()) {
            char buf[4]; snprintf(buf, 4, "%02x", op);
            e.location = frame_loc() + " op=0x" + buf;
        }
        throw;
    } catch (const std::runtime_error& e) {
        // Convert "InvalidRef:N" heap errors into catchable JvmExceptions so that
        // Java try-catch blocks (e.g. the Throwable handler in f.run()) can handle them.
        std::string msg = e.what();
        if (msg.rfind("InvalidRef:", 0) == 0) {
            // Re-enter the JvmException path so the exception table is consulted.
            JvmException jex{NULL_REF, "NullPointerException"};
            jex.location = frame_loc();
            // Retry the exception-table search inline
            uint32_t throw_pc = f.pc > 0 ? f.pc - 1 : 0;
            for (const auto& entry : ex_tbl) {
                if (throw_pc < entry.start_pc || throw_pc >= entry.end_pc) continue;
                bool matches = (entry.catch_type == 0);
                if (!matches && cf_ptr) {
                    const auto& cp_entry = cf_ptr->constant_pool.at(entry.catch_type);
                    if (const auto* cls = std::get_if<CpClass>(&cp_entry)) {
                        ClassDef* ck = m_loader.find_or_stub(cf_ptr->utf8(cls->name_index));
                        const std::string& cn = ck->name;
                        matches = (cn == "java/lang/Exception" || cn == "java/lang/RuntimeException" ||
                                   cn == "java/lang/Throwable" || cn == "java/lang/Error" ||
                                   cn == "java/lang/Object"    || cn == "java/lang/NullPointerException");
                    }
                }
                if (!matches) continue;
                ClassDef* ex_klass = m_loader.find_or_stub("java/lang/Exception");
                ObjRef ex_ref = m_heap.alloc_object(ex_klass, 0);
                f.sp = 0;
                f.push_ref(ex_ref);
                f.pc = entry.handler_pc;
                goto dispatch_restart;
            }
            throw jex;
        }
        char buf[4]; snprintf(buf, 4, "%02x", op);
        throw std::runtime_error(msg + "\n  in " + frame_loc() + " op=0x" + buf);
    }
    // continue outer loop after catch resumed via label below
    dispatch_restart: ;
    }  // end of outer while(true)
}
