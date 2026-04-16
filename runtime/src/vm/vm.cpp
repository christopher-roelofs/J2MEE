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
    int32_t  len    = arr->array_length();
    uint16_t* chars = arr->array_shorts();

    std::string out;
    out.reserve(len);
    for (int32_t i = 0; i < len; ++i)
        out += static_cast<char>(chars[i] & 0xFF);
    return out;
}

// ─── Constant pool resolution ─────────────────────────────────────────────────

static inline uint64_t cp_cache_key(const ClassFile& cf, uint16_t idx) {
    return (reinterpret_cast<uintptr_t>(&cf) << 16) | idx;
}

ClassDef* VM::resolve_class(const ClassFile& cf, uint16_t idx) {
    uint64_t k = cp_cache_key(cf, idx);
    auto it = m_class_cache.find(k);
    if (it != m_class_cache.end()) return it->second;

    const auto& entry = cf.constant_pool.at(idx);
    const auto* cls   = std::get_if<CpClass>(&entry);
    if (!cls) throw std::runtime_error("Expected CpClass at cp[" + std::to_string(idx) + "]");
    ClassDef* result = m_loader.find_or_stub(cf.utf8(cls->name_index));
    m_class_cache[k] = result;
    return result;
}

VM::FieldRef VM::resolve_field(const ClassFile& cf, uint16_t idx) {
    uint64_t k = cp_cache_key(cf, idx);
    auto it = m_field_cache.find(k);
    if (it != m_field_cache.end()) return it->second;

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
            FieldRef r{cur, fd};
            m_field_cache[k] = r;
            return r;
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
    FieldRef r{klass, &klass->static_fields.back()};
    m_field_cache[k] = r;
    return r;
}

VM::MethodRef VM::resolve_method(const ClassFile& cf, uint16_t idx) {
    uint64_t k = cp_cache_key(cf, idx);
    auto it = m_method_cache.find(k);
    if (it != m_method_cache.end()) return it->second;

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
        MethodRef r{klass, md};
        m_method_cache[k] = r;
        return r;
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
    MethodRef r{klass, klass->find_method(name, desc)};
    m_method_cache[k] = r;
    return r;
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

    m_call_stack.push_back(std::move(frame));
    exec_frame(m_call_stack.back());
    Frame completed = std::move(m_call_stack.back());
    m_call_stack.pop_back();

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

    initialize_class(klass);

    ObjRef midlet_obj = new_object(klass);

    // Call <init>()V
    if (auto* init = klass->resolve_virtual("<init>", "()V"))
        invoke(init, klass, {Slot::from_ref(midlet_obj)});

    // Call startApp()V — may be re-invoked after deferred threads complete
    // (mimics MIDP lifecycle: startApp → pause → resume → startApp again)
    MethodDef* startApp = klass->resolve_virtual("startApp", "()V");
    if (!startApp) throw std::runtime_error("startApp not found");

    for (int startApp_round = 0; startApp_round < 5; ++startApp_round) {
        invoke(startApp, klass, {Slot::from_ref(midlet_obj)});

        if (m_pending_threads.empty()) break;  // nothing deferred — done

        // Drain threads that were started during startApp / previous threads
        while (!m_pending_threads.empty()) {
            auto threads = std::move(m_pending_threads);
            m_pending_threads.clear();
            for (auto& pt : threads) {
                current_thread = pt.thread_ref;
                try {
                    invoke(pt.run_method, pt.run_klass, {Slot::from_ref(pt.runnable)});
                } catch (const QuitRequest&) {
                    current_thread = NULL_REF;
                    return;
                } catch (const JvmException& e) {
                    current_thread = NULL_REF;
                    fprintf(stderr, "[thread] run() threw JvmException: %s\n  at %s\n",
                            e.message.c_str(), e.location.c_str());
                } catch (const std::exception& e) {
                    current_thread = NULL_REF;
                    fprintf(stderr, "[thread] run() threw: %s\n", e.what());
                }
                current_thread = NULL_REF;
            }
        }
        // After threads complete, re-invoke startApp() — the game may now
        // be ready to proceed (e.g., ad SDK set a flag during its thread)
    }
}
