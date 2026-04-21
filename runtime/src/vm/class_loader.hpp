#pragma once
#include "class_def.hpp"
#include "classfile/class_file.hpp"
#include "util/jar.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

// ─── ClassLoader ─────────────────────────────────────────────────────────────
// Loads all classes from a JAR and builds the runtime ClassDef graph.
//
// Responsibilities:
//   1. Parse every .class file from the JAR
//   2. Build ClassDef for each (name, methods, fields)
//   3. Resolve the super / interface chain
//   4. Lay out instance field slots (including inherited fields)
//   5. Parse method descriptors (arg counts, return types)
//   6. Create stub ClassDefs for J2ME API classes (javax.*, java.*)
//      so reference resolution doesn't fail before we add native impls
//
// Native method bindings are registered separately via register_native().

class ClassLoader {
public:
    explicit ClassLoader(const JarFile& jar);

    // Same, plus a BIOS jar whose classes fill in any names the game jar
    // doesn't provide (MIDP/CLDC framework impls, e.g. phoneME sources).
    // The game jar always wins on name collisions.
    ClassLoader(const JarFile& game_jar, const JarFile& bios_jar);

    // Look up a loaded class by name (slash-separated).
    // Returns nullptr if the class is not known.
    ClassDef* find(const std::string& name);

    // Like find(), but creates a stub ClassDef if the class is unknown.
    // Used for J2ME API types that aren't in the JAR.
    ClassDef* find_or_stub(const std::string& name);

    // Bind a native implementation to a specific method. Flags select how
    // the binding interacts with an existing bytecode impl (e.g. from a
    // BIOS jar or the game itself):
    //   Override  — always wins (our C++ is authoritative; e.g. Graphics)
    //   FillGap   — defers to existing bytecode (for noop/stub registrations
    //               where BIOS or the game's real impl should take priority)
    enum class BindMode { Override, FillGap };

    void register_native(const std::string& class_name,
                         const std::string& method_name,
                         const std::string& descriptor,
                         NativeFunc fn,
                         BindMode mode = BindMode::Override);

    // All loaded (non-stub) classes, for initialisation ordering.
    std::vector<ClassDef*> all_classes();

private:
    // Parsed class files (owned here for lifetime)
    std::vector<std::unique_ptr<ClassFile>> m_class_files;

    // Runtime ClassDef map: name → ClassDef (owned here)
    std::unordered_map<std::string, std::unique_ptr<ClassDef>> m_classes;

    // ── Loading phases ───────────────────────────────────────────────────────

    // Phase 1: parse all .class entries from the JAR. `skip_existing` means
    // "additively fill in" — used for BIOS so game-provided classes win.
    void parse_classes(const JarFile& jar, bool skip_existing = false);

    // Phase 2: for each ClassDef, resolve super/interfaces pointers
    void link_hierarchy();

    // Phase 3: lay out instance field slots (DFS from java/lang/Object down)
    void layout_fields();
    void layout_fields_for(ClassDef* klass, std::unordered_set<ClassDef*>& visited);

    // Phase 4: parse method descriptors
    void resolve_methods();

    // ── Helpers ──────────────────────────────────────────────────────────────

    // Build a ClassDef from a parsed ClassFile (fields/methods, no linking yet)
    ClassDef* build_class_def(ClassFile* cf);

    // Create an empty stub ClassDef for a name we haven't loaded
    ClassDef* make_stub(const std::string& name);
};
