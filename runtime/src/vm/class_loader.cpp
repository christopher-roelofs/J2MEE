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

ClassLoader::ClassLoader(const JarFile& game_jar, const JarFile& bios_jar) {
    parse_classes(game_jar);
    size_t before = m_classes.size();
    parse_classes(bios_jar, /*skip_existing=*/true);
    size_t added = m_classes.size() - before;
    fprintf(stderr, "[bios] loaded %zu classes from BIOS jar (game provided %zu)\n",
            added, before);
    link_hierarchy();
    layout_fields();
    resolve_methods();
}

// ─── Phase 1: parse ───────────────────────────────────────────────────────────

// BIOS-only class filter. These are MIDP/CLDC supervisor classes whose
// bootstrap (suite loader, state handler, isolate manager) expects
// infrastructure we don't model — loading them drags the whole AMS into
// MIDlet.<init>. Games' direct MIDP API surface lives under javax/java/com.nokia
// etc. which are kept.
static bool is_bios_excluded(const std::string& name) {
    static const char* kExcludePrefixes[] = {
        "com/sun/midp/",          // MIDlet state handler, suite loader, AMS
        "com/sun/cldchi/",        // CLDC-HI VM internals
        "com/sun/j2me/",          // J2ME security/property handlers
        "com/sun/cdc/",           // CDC file/io handlers
        "com/sun/satsa/",         // smart-card APIs
        "org/recompile/",         // FreeJ2ME's own platform code
        "org/objectweb/asm/",     // bytecode manipulation; not needed
        "org/mozilla/",           // pluotsorbet-specific
        // BIOS RecordStore.java delegates to supervisor rms/RecordStoreFile
        // machinery we excluded; keep our native RMS impl instead.
        "javax/microedition/rms/",
    };
    for (auto* p : kExcludePrefixes)
        if (name.rfind(p, 0) == 0) return true;

    // Specific class exclusions. Some BIOS classes work; some pull supervisor
    // chains we filter out; some collide with comprehensive native impls we
    // already have. Exclude the ones where our native impl is a better answer
    // than BIOS's bytecode (BIOS uses instance fields, we use side-channel
    // maps — they don't compose).
    static const char* kExcludeExact[] = {
        // Supervisor-dependent
        "javax/microedition/lcdui/TextBox",
        "javax/microedition/lcdui/TextField",
        // Our native game-API impls (Sprite + Layer) conflict with BIOS bytecode
        "javax/microedition/lcdui/game/Sprite",
        "javax/microedition/lcdui/game/Layer",
        "javax/microedition/lcdui/game/LayerManager",
        "javax/microedition/lcdui/game/TiledLayer",
        // Our IO impls are complete; BIOS's would need supervisor (Reader,
        // ConnectionBaseAdapter, etc.) we filtered out.
        "java/io/DataInputStream",
        "java/io/DataOutputStream",
        "java/io/ByteArrayInputStream",
        "java/io/ByteArrayOutputStream",
    };
    for (auto* n : kExcludeExact)
        if (name == n) return true;
    return false;
}

void ClassLoader::parse_classes(const JarFile& jar, bool skip_existing) {
    for (auto& entry : jar.entries_with_suffix(".class")) {
        const auto& bytes = jar.get(entry);
        auto cf = std::make_unique<ClassFile>(parse_class_file(bytes));
        if (skip_existing) {
            if (m_classes.count(cf->this_class)) continue;
            if (is_bios_excluded(cf->this_class)) continue;
        }
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
    // find_or_stub() below inserts into m_classes, invalidating iterators of
    // an unordered_map traversal. Snapshot the JAR-loaded classes first and
    // iterate the snapshot so new stubs don't cut the loop short. (Before
    // this fix, only the first class that didn't need any new-stub parents
    // got its super set; every subsequent class ended up with super=null.
    // Visible effect: Super Puzzle Bobble's class `f` couldn't resolve
    // Canvas.repaint() because f → GAMECANVAS was never linked.)
    std::vector<ClassDef*> jar_classes;
    jar_classes.reserve(m_classes.size());
    for (auto& [name, klass] : m_classes)
        if (klass->source) jar_classes.push_back(klass.get());

    for (ClassDef* klass : jar_classes) {
        const ClassFile* cf = klass->source;
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
    ClassDef* ptr = klass.get();
    m_classes[name] = std::move(klass);

    // Seed well-known MIDP / CLDC parent links. Without this, a JAR-loaded
    // class that extends a stubbed framework class (e.g. Motorola
    // FullCanvas → GameCanvas → Canvas) terminates at the first stub when
    // resolve_virtual walks upward, so natives registered on Canvas never
    // get found and every invocation falls through to a generic stub.
    // Affected titles: Super Puzzle Bobble (com/hellomoto/fullscreen/game/
    // GAMECANVAS), other FullCanvas-based Motorola builds.
    static const std::pair<const char*, const char*> kStubParents[] = {
        {"javax/microedition/lcdui/game/GameCanvas",
            "javax/microedition/lcdui/Canvas"},
        {"javax/microedition/lcdui/Canvas",
            "javax/microedition/lcdui/Displayable"},
        {"javax/microedition/lcdui/Displayable", "java/lang/Object"},
        {"javax/microedition/lcdui/Screen",
            "javax/microedition/lcdui/Displayable"},
        {"javax/microedition/lcdui/Form",   "javax/microedition/lcdui/Screen"},
        {"javax/microedition/lcdui/List",   "javax/microedition/lcdui/Screen"},
        {"javax/microedition/lcdui/Alert",  "javax/microedition/lcdui/Screen"},
        {"javax/microedition/lcdui/TextBox","javax/microedition/lcdui/Screen"},
        // Nokia full-screen canvas used by many Asian ports of JSR-82 titles.
        {"com/nokia/mid/ui/FullCanvas", "javax/microedition/lcdui/Canvas"},
    };
    for (auto& [child, parent] : kStubParents) {
        if (name == child) {
            ptr->super = find_or_stub(parent);
            break;
        }
    }
    return ptr;
}

// ─── Native binding ───────────────────────────────────────────────────────────

void ClassLoader::register_native(const std::string& class_name,
                                  const std::string& method_name,
                                  const std::string& descriptor,
                                  NativeFunc fn,
                                  BindMode mode) {
    ClassDef* klass = find_or_stub(class_name);

    if (auto* md = klass->find_method(method_name, descriptor)) {
        bool declared_native = (md->access_flags & AccessFlags::NATIVE) != 0;
        // FillGap: someone else's bytecode impl (BIOS or game) wins; our
        // noop/stub only fires if no real impl exists OR the method was
        // declared native in its classfile (leaf primitive).
        if (mode == BindMode::FillGap && md->code && !declared_native) {
            if (std::getenv("J2ME_TRACE_BIOS"))
                fprintf(stderr,
                    "[bios] keeping bytecode impl for %s.%s%s (fill-gap stub deferred)\n",
                    class_name.c_str(), method_name.c_str(), descriptor.c_str());
            return;
        }
        // Override (default) or fill-gap-into-native-leaf: bind our impl.
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
