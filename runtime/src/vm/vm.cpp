#include "vm.hpp"
#include <algorithm>
#include <stdexcept>
#include <variant>

// ─── Construction ─────────────────────────────────────────────────────────────

VM::VM(const std::string& jar_path)
    : m_jar(jar_path)
    , m_heap(32 * 1024 * 1024)
    , m_loader(m_jar)
{
    m_object_class = m_loader.find_or_stub("java/lang/Object");
    m_string_class = m_loader.find_or_stub("java/lang/String");
    if (m_string_class->instance_slot_count < 2)
        m_string_class->instance_slot_count = 2;
}

VM::VM(const std::string& jar_path, const std::string& bios_jar_path)
    : m_jar(jar_path)
    , m_bios_jar(std::in_place, bios_jar_path)
    // BIOS adds ~788 framework classes whose <clinit>s eagerly allocate
    // (time-zone tables, default resource caches, etc.) — bump the heap.
    , m_heap(64 * 1024 * 1024)
    , m_loader(m_jar, *m_bios_jar)
{
    m_object_class = m_loader.find_or_stub("java/lang/Object");
    m_string_class = m_loader.find_or_stub("java/lang/String");
    if (m_string_class->instance_slot_count < 2)
        m_string_class->instance_slot_count = 2;
}

// ─── Class initialization ─────────────────────────────────────────────────────

void VM::initialize_class(ClassDef* klass) {
    if (!klass || klass->initialized) return;
    if (m_clinit_started.count(klass)) return;
    m_clinit_started.insert(klass);

    // Initialize super first
    if (klass->super)
        initialize_class(klass->super);

    klass->initialized = true;

    MethodDef* clinit = klass->find_method("<clinit>", "()V");
    if (!clinit) return;
    invoke(clinit, klass, std::span<const Slot>{});
}

// ─── Object creation ──────────────────────────────────────────────────────────

ObjRef VM::new_object(ClassDef* klass) {
    initialize_class(klass);
    ObjRef ref = m_heap.alloc_object(klass, klass->instance_slot_count);
    if (ref == NULL_REF)
        throw std::runtime_error("OutOfMemoryError: heap exhausted");
    return ref;
}

ObjRef VM::new_string(const std::string& utf8) {
    auto it = m_string_pool.find(utf8);
    if (it != m_string_pool.end()) return it->second;

    // Allocate char array (Java chars are UTF-16; we store raw bytes for now
    // since CLDC games are ASCII/Latin-1).
    ObjRef char_arr = m_heap.alloc_prim_array(
        ArrayType::Char,
        static_cast<int32_t>(utf8.size()),
        m_loader.find_or_stub("[C"));
    if (char_arr == NULL_REF)
        throw std::runtime_error("OutOfMemoryError");

    HeapObject* arr = m_heap.deref(char_arr);
    uint16_t* chars = arr->array_shorts();
    for (size_t i = 0; i < utf8.size(); ++i)
        chars[i] = static_cast<uint8_t>(utf8[i]);

    // Allocate String object with two fields: char[] value, int count
    // (simplified layout — real java.lang.String has more, but games access
    // it only through String methods which we implement natively)
    ObjRef str = m_heap.alloc_object(m_string_class, 2);
    if (str == NULL_REF) throw std::runtime_error("OutOfMemoryError");

    HeapObject* sobj = m_heap.deref(str);
    sobj->field(0) = Slot::from_ref(char_arr);
    sobj->field(1) = Slot::from_int(static_cast<int32_t>(utf8.size()));

    m_string_pool[utf8] = str;
    return str;
}

std::string VM::string_value(ObjRef ref) {
    if (ref == NULL_REF) return "(null)";
    HeapObject* sobj = m_heap.deref(ref);
    if (!sobj) return "(null)";

    // Objects without fields (stub exceptions, non-String objects) have no char array
    if (sobj->data_words == 0) return "";

    ObjRef char_arr = sobj->field(0).as_ref();
    if (char_arr == NULL_REF) return "";

    HeapObject* arr  = m_heap.deref(char_arr);
    if (!arr) return "";
    int32_t  len    = arr->array_length();
    if (len <= 0) return "";
    // Clamp: Java char[] length is bounded by heap size; anything above a
    // megabyte-ish here is heap corruption — don't trust it.
    if (len > (1 << 24)) len = 0;
    // Respect the String's count field (field(1)) as the logical length,
    // since games may grow the char[] beyond count.
    int32_t str_len = sobj->data_words >= 2 ? sobj->field(1).as_int() : len;
    if (str_len < 0 || str_len > len) str_len = len;
    uint16_t* chars = arr->array_shorts();

    std::string out;
    out.reserve(str_len);
    for (int32_t i = 0; i < str_len; ++i)
        out += static_cast<char>(chars[i] & 0xFF);
    return out;
}

// ─── Constant pool resolution ─────────────────────────────────────────────────

static inline uint64_t cp_cache_key(const ClassFile& cf, uint16_t idx) {
    return (reinterpret_cast<uintptr_t>(&cf) << 16) | idx;
}

ClassDef* VM::resolve_class(const ClassFile& cf, uint16_t idx) {
    // Per-ClassFile cache, see resolved_fields for the pattern.
    ClassDef*& slot = cf.resolved_classes[idx];
    if (slot) return slot;

    const auto& entry = cf.constant_pool.at(idx);
    const auto* cls   = std::get_if<CpClass>(&entry);
    if (!cls) throw std::runtime_error("Expected CpClass at cp[" + std::to_string(idx) + "]");
    slot = m_loader.find_or_stub(cf.utf8(cls->name_index));
    return slot;
}

VM::FieldRef VM::resolve_field_slow(const ClassFile& cf, uint16_t idx) {
    // Fast path (cache hit) is in vm.hpp so it inlines at each call site.
    // This is the cold path: walk the class hierarchy, install in the
    // per-CP cache, return.
    auto& slot = cf.resolved_fields[idx];

    const auto& entry = cf.constant_pool.at(idx);

    uint16_t class_idx, nat_idx;
    if (auto* fr = std::get_if<CpFieldref>(&entry)) {
        class_idx = fr->class_index;
        nat_idx   = fr->name_and_type_index;
    } else {
        throw std::runtime_error("Expected Fieldref at cp[" + std::to_string(idx) + "]");
    }

    ClassDef* klass = resolve_class(cf, class_idx);
    const auto& nat = cf.cp<CpNameAndType>(nat_idx);
    const std::string& name = cf.utf8(nat.name_index);
    const std::string& desc = cf.utf8(nat.descriptor_index);

    // Walk the class hierarchy to find the field (it may be in a super).
    ClassDef* cur = klass;
    while (cur) {
        if (auto* fd = cur->find_field(name, desc)) {
            slot = {cur, fd};
            return {cur, fd};
        }
        cur = cur->super;
    }

    // Not found: create a stub field so execution can continue
    FieldDef stub;
    stub.name         = name;
    stub.descriptor   = desc;
    stub.access_flags = AccessFlags::PUBLIC;
    stub.slot_index   = static_cast<uint32_t>(klass->static_fields.size());
    klass->static_fields.push_back(stub);
    klass->static_values.push_back(Slot{});
    slot = {klass, &klass->static_fields.back()};
    return {slot.first, slot.second};
}

VM::MethodRef VM::resolve_method_slow(const ClassFile& cf, uint16_t idx) {
    // See resolve_field_slow — fast path lives in the header.
    auto& slot = cf.resolved_methods[idx];

    const auto& entry = cf.constant_pool.at(idx);

    uint16_t class_idx, nat_idx;
    if (auto* mr = std::get_if<CpMethodref>(&entry)) {
        class_idx = mr->class_index;
        nat_idx   = mr->name_and_type_index;
    } else if (auto* im = std::get_if<CpIfaceMethod>(&entry)) {
        class_idx = im->class_index;
        nat_idx   = im->name_and_type_index;
    } else {
        throw std::runtime_error("Expected Methodref at cp[" + std::to_string(idx) + "]");
    }

    ClassDef* klass = resolve_class(cf, class_idx);
    const auto& nat = cf.cp<CpNameAndType>(nat_idx);
    const std::string& name = cf.utf8(nat.name_index);
    const std::string& desc = cf.utf8(nat.descriptor_index);

    if (auto* md = klass->resolve_virtual(name, desc)) {
        slot = {klass, md};
        return {klass, md};
    }

    // Stub: add a native method that returns a zero/null value of the right
    // type so the caller's stack stays consistent even before we implement it.
    auto ret = desc::ret_type(desc);
    std::string full = klass->name + "." + name + desc;
    m_loader.register_native(klass->name, name, desc,
        [full, ret](VM&, Frame& f, std::span<Slot>) {
            fprintf(stderr, "[stub] %s\n", full.c_str());
            switch (ret) {
                case MethodDef::RetType::Int:    f.push_int(0);      break;
                case MethodDef::RetType::Long:   f.push_long(0);     break;
                case MethodDef::RetType::Float:  f.push_float(0.0f); break;
                case MethodDef::RetType::Ref:    f.push_ref(NULL_REF); break;
                default: break;  // Void, Double
            }
        });
    slot = {klass, klass->find_method(name, desc)};
    return {slot.first, slot.second};
}

// ─── Invocation ───────────────────────────────────────────────────────────────

// Return 0, 1, or 2 slots from a completed frame based on the method return type.
static std::vector<Slot> slots_from_frame(Frame& fr, MethodDef* method) {
    using RT = MethodDef::RetType;
    auto result = [&]() -> std::vector<Slot> {
        switch (method->ret_type) {
        case RT::Void:   return {};
        case RT::Long:
        case RT::Double:
            if (fr.sp >= 2)
                return { fr.stack[fr.sp - 2], fr.stack[fr.sp - 1] };  // lo, hi
            return {};
        default:  // Int, Float, Ref
            if (fr.sp >= 1)
                return { fr.stack[fr.sp - 1] };
            return {};
        }
    }();
    // Debug: log when a non-void method returns 0 slots (unexpected)
    if (result.empty() && method->ret_type != RT::Void) {
        fprintf(stderr, "[vm] WARNING: %s.%s%s ret=%d sp=%u returned 0 slots!\n",
                method->owner ? method->owner->name.c_str() : "?",
                method->name.c_str(), method->descriptor.c_str(),
                (int)method->ret_type, fr.sp);
    }
    return result;
}

std::vector<Slot> VM::invoke(MethodDef* method, ClassDef* klass,
                             std::vector<Slot> args) {
    return invoke(method, klass, std::span<const Slot>(args));
}

std::vector<Slot> VM::invoke(MethodDef* method, ClassDef* klass,
                             std::span<const Slot> args) {
    if (method->is_native()) {
        if (!method->native_impl)
            throw std::runtime_error("Unimplemented native: " +
                                     klass->name + "." + method->name + method->descriptor);
        Frame tmp(method, klass);
        // native_impl takes span<Slot>; cast away const — natives may reuse
        // the buffer but our interpreter owns the backing memory.
        method->native_impl(*this, tmp,
                            std::span<Slot>(const_cast<Slot*>(args.data()), args.size()));
        return slots_from_frame(tmp, method);
    }

    if (!method->code)
        throw std::runtime_error("Abstract method called: " +
                                 klass->name + "." + method->name);

    Frame frame(method, klass);
    // Copy args into locals
    for (size_t i = 0; i < args.size(); ++i)
        frame.locals[i] = args[i];

    auto& stk = call_stack();
    stk.push_back(std::move(frame));
    try {
        exec_frame(stk.back());
    } catch (...) {
        // Exception propagating out of exec_frame: pop our frame first so
        // the caller's stk.back() resolves to the right outer frame.
        // Without this, subsequent invokes see a leaked frame and use it
        // as their "caller" — corrupting locals/sp of whichever frame
        // catches the exception downstream. Hit by Wolfenstein RPG when
        // openRecordStore throws RecordStoreNotFoundException through t.a.
        stk.pop_back();
        throw;
    }
    Frame completed = std::move(stk.back());
    stk.pop_back();

    return slots_from_frame(completed, method);
}

// ─── set_static ──────────────────────────────────────────────────────────────

void VM::set_static(const std::string& class_name, const std::string& field_name,
                    const std::string& desc, Slot value) {
    ClassDef* klass = m_loader.find_or_stub(class_name);
    FieldDef* fd    = klass->find_field(field_name, desc);
    if (fd) {
        klass->static_slot(fd->slot_index) = value;
        return;
    }
    // Field not yet seen — create it
    FieldDef nfd;
    nfd.name         = field_name;
    nfd.descriptor   = desc;
    nfd.access_flags = AccessFlags::PUBLIC | AccessFlags::STATIC;
    nfd.slot_index   = static_cast<uint32_t>(klass->static_values.size());
    klass->static_fields.push_back(nfd);
    klass->static_values.push_back(value);
}

// ─── run ─────────────────────────────────────────────────────────────────────

void VM::run(const std::string& midlet_class) {
    std::string class_name = midlet_class;
    std::replace(class_name.begin(), class_name.end(), '.', '/');
    ClassDef* klass = m_loader.find(class_name);
    if (!klass) throw std::runtime_error("MIDlet class not found: " + midlet_class);

    // Spawn a green thread that drives the MIDlet lifecycle (<init> →
    // startApp) on its own C stack. Any Thread.start() calls made during
    // <init> or startApp land in the scheduler's ready queue and run when
    // this thread yields via Thread.sleep() or Object.wait().
    scheduler.spawn(NULL_REF, [this, klass, class_name]() {
        initialize_class(klass);
        ObjRef midlet_obj = new_object(klass);
        fprintf(stderr, "[survey] midlet-constructed: %s\n", class_name.c_str());

        if (auto* init = klass->resolve_virtual("<init>", "()V"))
            invoke(init, klass, std::vector<Slot>{Slot::from_ref(midlet_obj)});

        MethodDef* startApp = klass->resolve_virtual("startApp", "()V");
        if (!startApp) throw std::runtime_error("startApp not found");

        fprintf(stderr, "[survey] startApp-entered round=0\n");
        invoke(startApp, klass, std::vector<Slot>{Slot::from_ref(midlet_obj)});
        fprintf(stderr, "[survey] startApp-returned round=0\n");
    });

    scheduler.run_to_completion();
}
