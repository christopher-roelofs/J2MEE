#pragma once
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

// ─── Constant Pool ───────────────────────────────────────────────────────────

enum class CpTag : uint8_t {
    Utf8              = 1,
    Integer           = 3,
    Float             = 4,
    Long              = 5,
    Double            = 6,
    Class             = 7,
    String            = 8,
    Fieldref          = 9,
    Methodref         = 10,
    InterfaceMethodref= 11,
    NameAndType       = 12,
};

struct CpUtf8        { std::string value; };
struct CpInteger     { int32_t value; };
struct CpFloat       { float value; };
struct CpLong        { int64_t value; };
struct CpDouble      { double value; };
struct CpClass       { uint16_t name_index; };       // → Utf8
struct CpString      { uint16_t string_index; };     // → Utf8
struct CpFieldref    { uint16_t class_index;   uint16_t name_and_type_index; };
struct CpMethodref   { uint16_t class_index;   uint16_t name_and_type_index; };
struct CpIfaceMethod { uint16_t class_index;   uint16_t name_and_type_index; };
struct CpNameAndType { uint16_t name_index;    uint16_t descriptor_index; };

using CpEntry = std::variant<
    std::monostate,   // index 0 / placeholder after long/double
    CpUtf8,
    CpInteger,
    CpFloat,
    CpLong,
    CpDouble,
    CpClass,
    CpString,
    CpFieldref,
    CpMethodref,
    CpIfaceMethod,
    CpNameAndType
>;

// ─── Attributes ──────────────────────────────────────────────────────────────

struct ExceptionTableEntry {
    uint16_t start_pc;
    uint16_t end_pc;
    uint16_t handler_pc;
    uint16_t catch_type;   // 0 = catch all (finally)
};

struct CodeAttribute {
    uint16_t max_stack;
    uint16_t max_locals;
    std::vector<uint8_t> code;
    std::vector<ExceptionTableEntry> exception_table;
};

struct RawAttribute {
    std::string name;
    std::vector<uint8_t> data;
};

// ─── Fields & Methods ────────────────────────────────────────────────────────

// Access flags (shared between class/field/method)
namespace AccessFlags {
    constexpr uint16_t PUBLIC       = 0x0001;
    constexpr uint16_t PRIVATE      = 0x0002;
    constexpr uint16_t PROTECTED    = 0x0004;
    constexpr uint16_t STATIC       = 0x0008;
    constexpr uint16_t FINAL        = 0x0010;
    constexpr uint16_t SYNCHRONIZED = 0x0020;
    constexpr uint16_t NATIVE       = 0x0100;
    constexpr uint16_t ABSTRACT     = 0x0400;
}

struct FieldInfo {
    uint16_t access_flags;
    std::string name;
    std::string descriptor;
    std::vector<RawAttribute> attributes;

    bool is_static() const { return access_flags & AccessFlags::STATIC; }
};

struct MethodInfo {
    uint16_t access_flags;
    std::string name;
    std::string descriptor;
    std::optional<CodeAttribute> code;
    std::vector<RawAttribute> attributes;

    bool is_static()  const { return access_flags & AccessFlags::STATIC; }
    bool is_native()  const { return access_flags & AccessFlags::NATIVE; }
    bool is_abstract()const { return access_flags & AccessFlags::ABSTRACT; }
};

// ─── ClassFile ───────────────────────────────────────────────────────────────

// Forward-declared runtime types. Populated lazily by VM::resolve_field
// and cached directly on the ClassFile — a single vector lookup on the
// bytecode's CP index replaces the unordered_map probe the global cache
// used to do. On a Spore title-screen profile (Pixel-7 class workload)
// resolve_field dropped from 39% of CPU to <1% with this change.
class ClassDef;
struct FieldDef;
struct MethodDef;

struct ClassFile {
    uint16_t minor_version;
    uint16_t major_version;

    std::vector<CpEntry> constant_pool;  // 1-indexed; [0] is monostate

    // Parallel to constant_pool, same length. Null klass = not yet resolved.
    // mutable: resolve_field takes `const ClassFile&` and writes here.
    mutable std::vector<std::pair<ClassDef*, FieldDef*>>  resolved_fields;
    mutable std::vector<std::pair<ClassDef*, MethodDef*>> resolved_methods;
    mutable std::vector<ClassDef*>                        resolved_classes;

    uint16_t access_flags;
    std::string this_class;              // resolved class name (slashes)
    std::string super_class;            // empty if java/lang/Object

    std::vector<std::string> interfaces;
    std::vector<FieldInfo>   fields;
    std::vector<MethodInfo>  methods;
    std::vector<RawAttribute> attributes;

    // Constant pool helpers
    const std::string& utf8(uint16_t idx) const;
    std::string class_name(uint16_t idx) const;  // resolves CpClass → name

    template<typename T>
    const T& cp(uint16_t idx) const {
        return std::get<T>(constant_pool.at(idx));
    }
};

// ─── Parser ──────────────────────────────────────────────────────────────────

// Load and parse a .class file from raw bytes.
ClassFile parse_class_file(std::span<const uint8_t> data);

// Convenience: load from disk
ClassFile load_class_file(const std::string& path);
