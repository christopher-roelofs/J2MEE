#pragma once
#include "object.hpp"
#include "value.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

// ─── Heap ────────────────────────────────────────────────────────────────────
// Handle-table GC heap.
//
// ObjRef is an index into m_table (1-based; 0 = null).
// Each entry is either a live HeapObject* or nullptr (freed/never used).
//
// Allocation strategy: bump allocator within 4 MB pages; GC is mark-sweep.
// For CLDC 1.1 the spec only guarantees 256 KB–2 MB of heap, so simplicity
// beats throughput here.

class Heap {
public:
    explicit Heap(size_t max_bytes = 4 * 1024 * 1024);
    ~Heap();

    // ── Allocation ───────────────────────────────────────────────────────────

    // Allocate a plain object with `slot_count` field slots.
    // Returns NULL_REF on OOM (you should GC and retry or throw OutOfMemory).
    ObjRef alloc_object(ClassDef* klass, uint32_t slot_count);

    // Allocate a primitive array (boolean/byte/char/short/int/float arrays).
    ObjRef alloc_prim_array(ArrayType type, int32_t length, ClassDef* arr_klass);

    // Allocate a reference array (Object[], String[], etc.).
    ObjRef alloc_ref_array(int32_t length, ClassDef* arr_klass);

    // Allocate a long/double array.
    ObjRef alloc_long_array(int32_t length, ClassDef* arr_klass);

    // ── Access ───────────────────────────────────────────────────────────────
    // Inlined hot-path: every getfield/putfield/arraystore funnels through
    // deref. Definition in the header so the compiler can inline the check.
    // Out-of-range refs throw — rare in correct code, so the slow path is
    // fine in a non-inline helper.

    [[gnu::always_inline]] HeapObject* deref(ObjRef ref) {
        if (__builtin_expect(ref == NULL_REF, 0)) return nullptr;
        if (__builtin_expect(ref >= m_table.size(), 0))
            return deref_slow(ref);
        return m_table[ref];
    }
    [[gnu::always_inline]] const HeapObject* deref(ObjRef ref) const {
        if (__builtin_expect(ref == NULL_REF, 0)) return nullptr;
        if (__builtin_expect(ref >= m_table.size(), 0))
            return deref_slow(ref);
        return m_table[ref];
    }

    bool is_null(ObjRef ref) const { return ref == NULL_REF; }
    bool valid(ObjRef ref)   const { return ref != NULL_REF && ref < m_table.size(); }

    // ── GC ───────────────────────────────────────────────────────────────────
    // Mark a set of root references, then sweep dead objects.
    // Roots are passed in by the caller (interpreter frames, static fields).
    void gc(const std::vector<ObjRef>& roots);

    size_t used_bytes()  const { return m_used; }
    size_t total_bytes() const { return m_max; }

private:
    // Out-of-range deref: throws. Non-inlined so the hot path stays small.
    HeapObject* deref_slow(ObjRef ref);
    const HeapObject* deref_slow(ObjRef ref) const;

    // Internal allocation from raw storage
    HeapObject* raw_alloc(size_t bytes);

    // GC phases
    void mark(ObjRef ref);
    void sweep();

    // Register a new HeapObject* in the handle table, return its ObjRef
    ObjRef register_object(HeapObject* obj);

    std::vector<HeapObject*> m_table;    // index 0 is unused (null slot)
    std::vector<uint32_t>    m_freelist; // recycled table indices

    // Raw storage: one big malloc'd slab
    uint8_t* m_storage  = nullptr;
    size_t   m_used     = 0;
    size_t   m_max      = 0;
};
