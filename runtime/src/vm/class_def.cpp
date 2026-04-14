#include "class_def.hpp"
#include <stdexcept>

// ─── FieldDef ────────────────────────────────────────────────────────────────

bool FieldDef::is_ref() const {
    return desc::is_ref(descriptor);
}

// ─── ClassDef ────────────────────────────────────────────────────────────────

MethodDef* ClassDef::find_method(const std::string& name, const std::string& d) {
    for (auto& m : methods)
        if (m.name == name && m.descriptor == d)
            return &m;
    return nullptr;
}

const MethodDef* ClassDef::find_method(const std::string& name, const std::string& d) const {
    for (auto& m : methods)
        if (m.name == name && m.descriptor == d)
            return &m;
    return nullptr;
}

FieldDef* ClassDef::find_field(const std::string& name, const std::string& d) {
    for (auto& f : instance_fields)
        if (f.name == name && f.descriptor == d)
            return &f;
    for (auto& f : static_fields)
        if (f.name == name && f.descriptor == d)
            return &f;
    return nullptr;
}

MethodDef* ClassDef::resolve_virtual(const std::string& name, const std::string& d) {
    ClassDef* cur = this;
    while (cur) {
        if (auto* m = cur->find_method(name, d))
            return m;
        cur = cur->super;
    }
    return nullptr;
}

bool ClassDef::is_subclass_of(const ClassDef* other) const {
    const ClassDef* cur = this;
    while (cur) {
        if (cur == other) return true;
        cur = cur->super;
    }
    return false;
}

bool ClassDef::implements(const ClassDef* iface) const {
    for (auto* i : interfaces)
        if (i == iface || i->implements(iface))
            return true;
    if (super) return super->implements(iface);
    return false;
}

// ─── Descriptor parsing ───────────────────────────────────────────────────────

namespace desc {

// Parse one type from `descriptor` starting at `pos`, advance pos past it.
// Returns the number of slots that type occupies.
static uint32_t parse_one_type(const std::string& d, size_t& pos) {
    if (pos >= d.size())
        throw std::runtime_error("Truncated descriptor: " + d);

    char c = d[pos++];
    switch (c) {
        case 'B': case 'C': case 'F':
        case 'I': case 'S': case 'Z':
            return 1;
        case 'D': case 'J':
            return 2;
        case 'L':
            while (pos < d.size() && d[pos] != ';') ++pos;
            ++pos;  // consume ';'
            return 1;
        case '[':
            parse_one_type(d, pos);  // skip element type
            return 1;
        case 'V':
            return 0;
        default:
            throw std::runtime_error(std::string("Unknown type in descriptor: ") + c);
    }
}

uint32_t arg_slots(const std::string& descriptor) {
    if (descriptor.empty() || descriptor[0] != '(')
        throw std::runtime_error("Invalid method descriptor: " + descriptor);

    size_t pos = 1;
    uint32_t total = 0;
    while (pos < descriptor.size() && descriptor[pos] != ')')
        total += parse_one_type(descriptor, pos);
    return total;
}

MethodDef::RetType ret_type(const std::string& descriptor) {
    size_t rparen = descriptor.find(')');
    if (rparen == std::string::npos)
        throw std::runtime_error("Invalid method descriptor: " + descriptor);

    char c = descriptor[rparen + 1];
    switch (c) {
        case 'V': return MethodDef::RetType::Void;
        case 'J': return MethodDef::RetType::Long;
        case 'D': return MethodDef::RetType::Double;
        case 'F': return MethodDef::RetType::Float;
        case 'L': case '[': return MethodDef::RetType::Ref;
        default:  return MethodDef::RetType::Int;
    }
}

bool is_ref(const std::string& descriptor) {
    return !descriptor.empty() && (descriptor[0] == 'L' || descriptor[0] == '[');
}

bool is_wide(const std::string& descriptor) {
    return descriptor == "J" || descriptor == "D";
}

}  // namespace desc
