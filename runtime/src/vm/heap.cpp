#include "heap.hpp"
#include "class_def.hpp"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

// ─── Helpers ─────────────────────────────────────────────────────────────────

// Round up to pointer alignment (8 bytes to keep HeapObject header aligned)
static constexpr size_t align8(size_t n) { return (n + 7u) & ~7u; }

// Header size in bytes (must be a multiple of 8 for alignment)
static_assert(sizeof(HeapObject) % 8 == 0 || sizeof(HeapObject) <= 16,
              "HeapObject header should be compact");

// ─── Construction ─────────────────────────────────────────────────────────────

Heap::Heap(size_t max_bytes) : m_max(max_bytes) {
    m_storage = static_cast<uint8_t*>(std::malloc(max_bytes));
    if (!m_storage)
        throw std::bad_alloc{};

    // Index 0 is the null slot — never used for a real object
    m_table.push_back(nullptr);
}

Heap::~Heap() {
    std::free(m_storage);
}

// ─── Raw allocation ───────────────────────────────────────────────────────────

HeapObject* Heap::raw_alloc(size_t bytes) {
    size_t aligned = align8(bytes);
    if (m_used + aligned > m_max)
        return nullptr;  // OOM — caller should trigger GC

    auto* obj = reinterpret_cast<HeapObject*>(m_storage + m_used);
    m_used += aligned;
    std::memset(obj, 0, aligned);
    return obj;
}

ObjRef Heap::register_object(HeapObject* obj) {
    if (!m_freelist.empty()) {
        uint32_t idx = m_freelist.back();
        m_freelist.pop_back();
        m_table[idx] = obj;
        return static_cast<ObjRef>(idx);
    }
    uint32_t idx = static_cast<uint32_t>(m_table.size());
    m_table.push_back(obj);
    return static_cast<ObjRef>(idx);
}

// ─── Allocation API ──────────────────────────────────────────────────────────

ObjRef Heap::alloc_object(ClassDef* klass, uint32_t slot_count) {
    size_t bytes = sizeof(HeapObject) + slot_count * sizeof(Slot);
    HeapObject* obj = raw_alloc(bytes);
    if (!obj) return NULL_REF;

    obj->klass      = klass;
    obj->gc_flags   = 0;
    obj->data_words = slot_count;
    // Fields are already zeroed (null / 0) by memset
    return register_object(obj);
}

ObjRef Heap::alloc_prim_array(ArrayType type, int32_t length, ClassDef* arr_klass) {
    if (length < 0)
        throw std::runtime_error("Negative array size");

    size_t elem_bytes;
    switch (type) {
        case ArrayType::Boolean:
        case ArrayType::Byte:    elem_bytes = 1; break;
        case ArrayType::Char:
        case ArrayType::Short:   elem_bytes = 2; break;
        case ArrayType::Float:
        case ArrayType::Int:     elem_bytes = 4; break;
        case ArrayType::Double:
        case ArrayType::Long:    elem_bytes = 8; break;
        default:                 elem_bytes = 4; break;
    }

    // Header + int32_t length field + element data
    size_t data_bytes = sizeof(int32_t) + static_cast<size_t>(length) * elem_bytes;
    size_t total = sizeof(HeapObject) + data_bytes;

    HeapObject* obj = raw_alloc(total);
    if (!obj) return NULL_REF;

    obj->klass      = arr_klass;
    obj->gc_flags   = 0;
    obj->data_words = static_cast<uint32_t>(align8(data_bytes) / sizeof(int32_t));
    obj->array_length() = length;
    // Elements already zeroed
    return register_object(obj);
}

ObjRef Heap::alloc_ref_array(int32_t length, ClassDef* arr_klass) {
    if (length < 0)
        throw std::runtime_error("Negative array size");

    // Header + int32_t length + ObjRef elements (each is int32_t / Slot)
    size_t data_bytes = sizeof(int32_t) + static_cast<size_t>(length) * sizeof(Slot);
    HeapObject* obj = raw_alloc(sizeof(HeapObject) + data_bytes);
    if (!obj) return NULL_REF;

    obj->klass      = arr_klass;
    obj->gc_flags   = 0;
    obj->data_words = static_cast<uint32_t>(align8(data_bytes) / sizeof(int32_t));
    obj->array_length() = length;
    return register_object(obj);
}

ObjRef Heap::alloc_long_array(int32_t length, ClassDef* arr_klass) {
    if (length < 0)
        throw std::runtime_error("Negative array size");

    size_t data_bytes = sizeof(int32_t) + static_cast<size_t>(length) * sizeof(int64_t);
    HeapObject* obj = raw_alloc(sizeof(HeapObject) + data_bytes);
    if (!obj) return NULL_REF;

    obj->klass      = arr_klass;
    obj->gc_flags   = 0;
    obj->data_words = static_cast<uint32_t>(align8(data_bytes) / sizeof(int32_t));
    obj->array_length() = length;
    return register_object(obj);
}

// ─── Access ───────────────────────────────────────────────────────────────────

HeapObject* Heap::deref_slow(ObjRef ref) {
    throw std::runtime_error("InvalidRef:" + std::to_string(ref));
}

const HeapObject* Heap::deref_slow(ObjRef ref) const {
    throw std::runtime_error("InvalidRef:" + std::to_string(ref));
}

// ─── GC: mark-sweep ──────────────────────────────────────────────────────────

void Heap::mark(ObjRef ref) {
    if (ref == NULL_REF) return;

    HeapObject* obj = m_table[ref];
    if (!obj || obj->is_marked()) return;
    obj->set_mark();

    // If this is an instance object, scan its fields for references.
    // Arrays of references also need scanning.
    // We rely on klass to tell us the layout.
    if (!obj->klass) return;  // shouldn't happen for well-formed objects

    // Ask the class how many slots are reference slots.
    // For now: scan ALL slots and treat anything non-null as a potential ref.
    // A proper implementation would consult ClassDef::ref_map.
    // This conservative scan is safe (may keep some ints alive if they
    // coincidentally match a live ObjRef, but won't miss real refs).
    uint32_t n = obj->data_words;
    const Slot* slots = obj->field_slots();
    for (uint32_t i = 0; i < n; ++i) {
        ObjRef child = static_cast<ObjRef>(slots[i].raw);
        if (child != NULL_REF && child < m_table.size() && m_table[child])
            mark(child);
    }
}

void Heap::sweep() {
    // Walk the handle table; free any unmarked live objects and recycle slots.
    // We can't physically free from the bump allocator slab (no per-object
    // free), so for now this just clears the handle — the slab memory is
    // "leaked" until we implement compaction.  For CLDC heap sizes this is
    // acceptable during early development.
    for (uint32_t i = 1; i < m_table.size(); ++i) {
        HeapObject* obj = m_table[i];
        if (!obj) continue;

        if (obj->is_marked()) {
            obj->clear_mark();  // reset for next cycle
        } else {
            m_table[i] = nullptr;
            m_freelist.push_back(i);
        }
    }
}

void Heap::gc(const std::vector<ObjRef>& roots) {
    // Mark phase
    for (ObjRef r : roots)
        mark(r);

    // Sweep phase
    sweep();
}
