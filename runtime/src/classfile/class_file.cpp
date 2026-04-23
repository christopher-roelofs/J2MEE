#include "class_file.hpp"
#include "class_reader.hpp"

#include <bit>
#include <cstring>
#include <fstream>
#include <stdexcept>

// ─── Constant Pool helpers ────────────────────────────────────────────────────

const std::string& ClassFile::utf8(uint16_t idx) const {
    return cp<CpUtf8>(idx).value;
}

std::string ClassFile::class_name(uint16_t idx) const {
    return utf8(cp<CpClass>(idx).name_index);
}

// ─── Attribute parsing ────────────────────────────────────────────────────────

static CodeAttribute parse_code(ClassReader& r, const ClassFile& cf) {
    CodeAttribute code;
    code.max_stack  = r.u2();
    code.max_locals = r.u2();

    uint32_t code_len = r.u4();
    code.code = r.bytes(code_len);

    uint16_t ex_count = r.u2();
    code.exception_table.reserve(ex_count);
    for (uint16_t i = 0; i < ex_count; ++i) {
        ExceptionTableEntry e;
        e.start_pc   = r.u2();
        e.end_pc     = r.u2();
        e.handler_pc = r.u2();
        e.catch_type = r.u2();
        code.exception_table.push_back(e);
    }

    // Skip inner attributes (LineNumberTable, LocalVariableTable, etc.)
    uint16_t attr_count = r.u2();
    for (uint16_t i = 0; i < attr_count; ++i) {
        r.u2();                          // name_index
        uint32_t len = r.u4();
        r.bytes(len);                    // discard
    }

    return code;
}

static RawAttribute parse_raw_attribute(ClassReader& r, const ClassFile& cf) {
    uint16_t name_idx = r.u2();
    uint32_t length   = r.u4();
    RawAttribute attr;
    attr.name = cf.utf8(name_idx);
    attr.data = r.bytes(length);
    return attr;
}

// ─── Field / Method parsing ───────────────────────────────────────────────────

static FieldInfo parse_field(ClassReader& r, const ClassFile& cf) {
    FieldInfo f;
    f.access_flags = r.u2();
    f.name         = cf.utf8(r.u2());
    f.descriptor   = cf.utf8(r.u2());

    uint16_t attr_count = r.u2();
    for (uint16_t i = 0; i < attr_count; ++i)
        f.attributes.push_back(parse_raw_attribute(r, cf));

    return f;
}

static MethodInfo parse_method(ClassReader& r, const ClassFile& cf) {
    MethodInfo m;
    m.access_flags = r.u2();
    m.name         = cf.utf8(r.u2());
    m.descriptor   = cf.utf8(r.u2());

    uint16_t attr_count = r.u2();
    for (uint16_t i = 0; i < attr_count; ++i) {
        uint16_t name_idx = r.u2();
        uint32_t length   = r.u4();
        std::string attr_name = cf.utf8(name_idx);

        if (attr_name == "Code") {
            // Inline parse rather than going through raw bytes
            // Temporarily build a sub-reader over the attribute body
            auto body = r.bytes(length);
            ClassReader sub({body.data(), body.size()});
            m.code = parse_code(sub, cf);
        } else {
            RawAttribute raw;
            raw.name = std::move(attr_name);
            raw.data = r.bytes(length);
            m.attributes.push_back(std::move(raw));
        }
    }

    return m;
}

// ─── Constant Pool parsing ────────────────────────────────────────────────────

static std::vector<CpEntry> parse_constant_pool(ClassReader& r, uint16_t count) {
    // count is constant_pool_count; valid indices are 1..(count-1)
    std::vector<CpEntry> pool(count);   // [0] stays monostate

    uint16_t i = 1;
    while (i < count) {
        uint8_t tag = r.u1();
        switch (static_cast<CpTag>(tag)) {
            case CpTag::Utf8: {
                uint16_t len = r.u2();
                auto raw = r.bytes(len);
                // MUTF-8: for CLDC games this is effectively ASCII/Latin-1
                pool[i] = CpUtf8{std::string(raw.begin(), raw.end())};
                break;
            }
            case CpTag::Integer:
                pool[i] = CpInteger{static_cast<int32_t>(r.u4())};
                break;
            case CpTag::Float: {
                uint32_t bits = r.u4();
                float f;
                std::memcpy(&f, &bits, 4);
                pool[i] = CpFloat{f};
                break;
            }
            case CpTag::Long:
                pool[i] = CpLong{static_cast<int64_t>(r.u8())};
                ++i;  // longs/doubles occupy two slots
                pool[i] = std::monostate{};
                break;
            case CpTag::Double: {
                uint64_t bits = r.u8();
                double d;
                std::memcpy(&d, &bits, 8);
                pool[i] = CpDouble{d};
                ++i;
                pool[i] = std::monostate{};
                break;
            }
            case CpTag::Class:
                pool[i] = CpClass{r.u2()};
                break;
            case CpTag::String:
                pool[i] = CpString{r.u2()};
                break;
            case CpTag::Fieldref:
                pool[i] = CpFieldref{r.u2(), r.u2()};
                break;
            case CpTag::Methodref:
                pool[i] = CpMethodref{r.u2(), r.u2()};
                break;
            case CpTag::InterfaceMethodref:
                pool[i] = CpIfaceMethod{r.u2(), r.u2()};
                break;
            case CpTag::NameAndType:
                pool[i] = CpNameAndType{r.u2(), r.u2()};
                break;
            // Java 7+ tags — skip the payload so modern class files load.
            // CLDC games don't actually use invokedynamic; if bytecode does
            // reference these, the interpreter will fail at decode time,
            // which is better than refusing the whole class.
            case static_cast<CpTag>(15):  // MethodHandle: u1+u2
                r.u1(); r.u2();
                pool[i] = std::monostate{};
                break;
            case static_cast<CpTag>(16):  // MethodType: u2
                r.u2();
                pool[i] = std::monostate{};
                break;
            case static_cast<CpTag>(17):  // Dynamic: u2+u2
            case static_cast<CpTag>(18):  // InvokeDynamic: u2+u2
                r.u2(); r.u2();
                pool[i] = std::monostate{};
                break;
            case static_cast<CpTag>(19):  // Module: u2
            case static_cast<CpTag>(20):  // Package: u2
                r.u2();
                pool[i] = std::monostate{};
                break;
            default:
                throw std::runtime_error("Unknown constant pool tag: " +
                                         std::to_string(tag) + " at index " +
                                         std::to_string(i));
        }
        ++i;
    }
    return pool;
}

// ─── Top-level parser ─────────────────────────────────────────────────────────

ClassFile parse_class_file(std::span<const uint8_t> data) {
    ClassReader r(data);
    ClassFile cf;

    uint32_t magic = r.u4();
    if (magic != 0xCAFEBABE)
        throw std::runtime_error("Not a class file (bad magic)");

    cf.minor_version = r.u2();
    cf.major_version = r.u2();

    uint16_t cp_count = r.u2();
    cf.constant_pool = parse_constant_pool(r, cp_count);
    // Lazy resolution caches; populated on first GETFIELD/INVOKESTATIC/etc.
    // targeting each CP index. Null klass = not yet resolved.
    const size_t n = cf.constant_pool.size();
    cf.resolved_fields.assign(n,  {nullptr, nullptr});
    cf.resolved_methods.assign(n, {nullptr, nullptr});
    cf.resolved_classes.assign(n, nullptr);

    cf.access_flags = r.u2();
    cf.this_class   = cf.class_name(r.u2());

    uint16_t super_idx = r.u2();
    cf.super_class = (super_idx == 0) ? "" : cf.class_name(super_idx);

    uint16_t iface_count = r.u2();
    cf.interfaces.reserve(iface_count);
    for (uint16_t i = 0; i < iface_count; ++i)
        cf.interfaces.push_back(cf.class_name(r.u2()));

    uint16_t field_count = r.u2();
    cf.fields.reserve(field_count);
    for (uint16_t i = 0; i < field_count; ++i)
        cf.fields.push_back(parse_field(r, cf));

    uint16_t method_count = r.u2();
    cf.methods.reserve(method_count);
    for (uint16_t i = 0; i < method_count; ++i)
        cf.methods.push_back(parse_method(r, cf));

    uint16_t attr_count = r.u2();
    cf.attributes.reserve(attr_count);
    for (uint16_t i = 0; i < attr_count; ++i)
        cf.attributes.push_back(parse_raw_attribute(r, cf));

    return cf;
}

ClassFile load_class_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        throw std::runtime_error("Cannot open: " + path);

    std::vector<uint8_t> data{
        std::istreambuf_iterator<char>(f),
        std::istreambuf_iterator<char>()
    };
    return parse_class_file(data);
}
