#include "class_loader.hpp"
#include <stdexcept>
#include <unordered_set>

// ─── Construction ─────────────────────────────────────────────────────────────

ClassLoader::ClassLoader(const JarFile& jar) {
    parse_classes(jar);
    link_hierarchy();
    layout_fields();
    resolve_methods();
}

// ─── Phase 1: parse ───────────────────────────────────────────────────────────

void ClassLoader::parse_classes(const JarFile& jar) {
    for (auto& entry : jar.entries_with_suffix(".class")) {
        const auto& bytes = jar.get(entry);
        auto cf = std::make_unique<ClassFile>(parse_class_file(bytes));
        ClassFile* raw = cf.get();
        m_class_files.push_back(std::move(cf));
        build_class_def(raw);
    }
}

ClassDef* ClassLoader::build_class_def(ClassFile* cf) {
    auto klass = std::make_unique<ClassDef>();
    klass->name   = cf->this_class;
    klass->source = cf;

    // ── Instance fields ───────────────────────────────────────────────────────
    // slot_index assigned during layout_fields (phase 3), not here.
    for (auto& fi : cf->fields) {
        FieldDef fd;
        fd.name         = fi.name;
        fd.descriptor   = fi.descriptor;
        fd.access_flags = fi.access_flags;
        fd.slot_index   = 0;  // filled in phase 3

        if (fi.is_static())
            klass->static_fields.push_back(fd);
        else
            klass->instance_fields.push_back(fd);
    }

    // ── Methods ───────────────────────────────────────────────────────────────
    for (auto& mi : cf->methods) {
        MethodDef md;
        md.name         = mi.name;
        md.descriptor   = mi.descriptor;
        md.access_flags = mi.access_flags;
        md.owner        = klass.get();
        md.code         = mi.code ? &(*mi.code) : nullptr;
        // arg_slot_count and ret_type filled in phase 4

        klass->methods.push_back(std::move(md));
    }

    ClassDef* ptr = klass.get();
    m_classes[cf->this_class] = std::move(klass);
    return ptr;
}

// ─── Phase 2: link hierarchy ──────────────────────────────────────────────────

void ClassLoader::link_hierarchy() {
    for (auto& [name, klass] : m_classes) {
        const ClassFile* cf = klass->source;
        if (!cf) continue;

        if (!cf->super_class.empty())
            klass->super = find_or_stub(cf->super_class);

        for (auto& iname : cf->interfaces)
            klass->interfaces.push_back(find_or_stub(iname));
    }
}

// ─── Phase 3: field layout ────────────────────────────────────────────────────
//
// Walk the inheritance chain super-first so that a subclass's fields start
// at the slot after its parent's fields (mirroring real JVM layout).

void ClassLoader::layout_fields() {
    // Assign static field slot indices first (per-class, independent)
    for (auto& [name, klass] : m_classes) {
        uint32_t idx = 0;
        for (auto& fd : klass->static_fields) {
            fd.slot_index = idx;
            idx += fd.slot_count();
        }
        klass->static_values.resize(idx);  // zero-initialised
    }

    // Instance field layout: need to visit super before sub
    std::unordered_set<ClassDef*> visited;
    for (auto& [name, klass] : m_classes)
        layout_fields_for(klass.get(), visited);
}

void ClassLoader::layout_fields_for(ClassDef* klass,
                                    std::unordered_set<ClassDef*>& visited) {
    if (visited.count(klass)) return;
    visited.insert(klass);

    // Ensure super is laid out first
    uint32_t base = 0;
    if (klass->super) {
        layout_fields_for(klass->super, visited);
        base = klass->super->instance_slot_count;
    }

    uint32_t next = base;
    for (auto& fd : klass->instance_fields) {
        fd.slot_index = next;
        next += fd.slot_count();
    }
    klass->instance_slot_count = next;
}

// ─── Phase 4: method descriptors ──────────────────────────────────────────────

void ClassLoader::resolve_methods() {
    for (auto& [name, klass] : m_classes) {
        for (auto& md : klass->methods) {
            md.arg_slot_count = desc::arg_slots(md.descriptor);
            md.ret_type       = desc::ret_type(md.descriptor);
        }
    }
}

// ─── Lookup ───────────────────────────────────────────────────────────────────

ClassDef* ClassLoader::find(const std::string& name) {
    auto it = m_classes.find(name);
    return it != m_classes.end() ? it->second.get() : nullptr;
}

ClassDef* ClassLoader::find_or_stub(const std::string& name) {
    if (auto* klass = find(name)) return klass;
    return make_stub(name);
}

ClassDef* ClassLoader::make_stub(const std::string& name) {
    auto klass = std::make_unique<ClassDef>();
    klass->name   = name;
    klass->source = nullptr;
    // Stub the most common base class chain so instanceof / is_subclass_of works
    // for the J2ME hierarchy.  Full hierarchy is built by native registration.
    ClassDef* ptr = klass.get();
    m_classes[name] = std::move(klass);
    return ptr;
}

// ─── Native binding ───────────────────────────────────────────────────────────

void ClassLoader::register_native(const std::string& class_name,
                                  const std::string& method_name,
                                  const std::string& descriptor,
                                  NativeFunc fn) {
    ClassDef* klass = find_or_stub(class_name);

    // If the method already exists (from a loaded class file), bind to it
    // and force the NATIVE flag so the interpreter dispatches to native_impl
    // instead of the original bytecode.
    if (auto* md = klass->find_method(method_name, descriptor)) {
        md->native_impl   = std::move(fn);
        md->access_flags  = md->access_flags | AccessFlags::NATIVE;
        return;
    }

    // Otherwise add a new native-only MethodDef (for pure-stub classes)
    MethodDef md;
    md.name           = method_name;
    md.descriptor     = descriptor;
    md.access_flags   = AccessFlags::PUBLIC | AccessFlags::NATIVE;
    md.owner          = klass;
    md.arg_slot_count = desc::arg_slots(descriptor);
    md.ret_type       = desc::ret_type(descriptor);
    md.native_impl    = std::move(fn);
    klass->methods.push_back(std::move(md));
}

std::vector<ClassDef*> ClassLoader::all_classes() {
    std::vector<ClassDef*> out;
    out.reserve(m_classes.size());
    for (auto& [name, klass] : m_classes)
        if (klass->source)  // skip stubs
            out.push_back(klass.get());
    return out;
}
