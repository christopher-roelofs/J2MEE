#include "natives.hpp"
#include "vm/vm.hpp"
#include "vm/heap.hpp"
#include "util/jar.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unordered_map>

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_ttf.h>
#include "backend/display.hpp"

namespace fs = std::filesystem;

// Defined in graphics_natives.cpp
extern int g_screen_w, g_screen_h;
TTF_Font* get_ttf_font(int px_size, bool bold, bool mono = false);  // defined in graphics_natives.cpp

// ─── UTF-8 ↔ character-index helpers ─────────────────────────────────────────
// Java strings are UTF-16 character-indexed.  We store UTF-8 internally, so we
// need to convert between byte offsets and character indices.

// Count the number of Unicode code points in a UTF-8 string.
static int32_t utf8_char_count(const std::string& s) {
    int32_t count = 0;
    for (size_t i = 0; i < s.size(); ) {
        uint8_t c = s[i];
        if      (c < 0x80)   i += 1;
        else if (c < 0xE0)   i += 2;
        else if (c < 0xF0)   i += 3;
        else                  i += 4;
        ++count;
    }
    return count;
}

// Convert a character index to a byte offset.  Returns s.size() if idx >= char count.
static size_t utf8_char_to_byte(const std::string& s, int32_t char_idx) {
    size_t byte_off = 0;
    int32_t ci = 0;
    while (byte_off < s.size() && ci < char_idx) {
        uint8_t c = s[byte_off];
        if      (c < 0x80)   byte_off += 1;
        else if (c < 0xE0)   byte_off += 2;
        else if (c < 0xF0)   byte_off += 3;
        else                  byte_off += 4;
        ++ci;
    }
    return byte_off;
}

// Decode the Unicode code point at a byte offset.  Advances off past the character.
static uint16_t utf8_decode_at(const std::string& s, size_t& off) {
    if (off >= s.size()) return 0;
    uint8_t c = s[off];
    uint16_t cp;
    if (c < 0x80) {
        cp = c; off += 1;
    } else if (c < 0xE0) {
        cp = ((c & 0x1F) << 6) | (s[off+1] & 0x3F);
        off += 2;
    } else if (c < 0xF0) {
        cp = ((c & 0x0F) << 12) | ((s[off+1] & 0x3F) << 6) | (s[off+2] & 0x3F);
        off += 3;
    } else {
        // 4-byte chars → surrogate pair territory; return replacement char
        cp = 0xFFFD; off += 4;
    }
    return cp;
}

// Extract a character-indexed substring [from, to) as a UTF-8 std::string.
static std::string utf8_substring(const std::string& s, int32_t from, int32_t to) {
    size_t byte_from = utf8_char_to_byte(s, from);
    size_t byte_to   = utf8_char_to_byte(s, to);
    if (byte_from > s.size()) byte_from = s.size();
    if (byte_to   > s.size()) byte_to   = s.size();
    return s.substr(byte_from, byte_to - byte_from);
}

// ─── External storage for native-backed types ─────────────────────────────────
// Java standard library objects (String, StringBuffer, Hashtable, etc.) exist
// on the heap as empty shells (0-slot stubs).  Their actual data lives here,
// keyed by ObjRef.

static std::unordered_map<ObjRef, StreamEntry>   g_streams;
StreamEntry* find_stream(ObjRef ref) {
    auto it = g_streams.find(ref);
    return it == g_streams.end() ? nullptr : &it->second;
}
std::unordered_map<ObjRef, std::string>   g_string_buffers;
static std::unordered_map<ObjRef, int32_t>       g_integers;
// Hashtable: key ObjRef -> (map of key_hash -> pair<ObjRef,ObjRef>)
// For simplicity we use a vector of pairs and do linear search on equals.
using KVPair = std::pair<ObjRef, ObjRef>;
static std::unordered_map<ObjRef, std::vector<KVPair>> g_hashtables;
std::unordered_map<ObjRef, std::vector<ObjRef>> g_vectors;
static std::unordered_map<ClassDef*, ObjRef>           g_class_objects;
// Command listener storage (for all Displayables including List)
std::unordered_map<ObjRef, ObjRef> g_command_listeners;

// MIDP List UI: stores items for List Displayables
struct ListData {
    std::string title;
    int type = 0;  // IMPLICIT=3, EXCLUSIVE=1, MULTIPLE=2
    std::vector<std::string> items;
    int selected = 0;
    ObjRef command_listener = NULL_REF;
    ObjRef select_command = NULL_REF;
};
static std::unordered_map<ObjRef, ListData> g_lists;

// RecordStore: ObjRef -> store name (lookup into g_record_stores)
static std::unordered_map<ObjRef, std::string>         g_recordstore_names;
static std::unordered_map<std::string, std::vector<std::vector<uint8_t>>> g_record_stores;
static std::string g_rms_dir;  // persistent storage directory

// ─── RecordStore persistence ─────────────────────────────────────────────────
// File format per store: [u32 num_records] then for each record:
//   [u32 length] [length bytes]    (length=0xFFFFFFFF means deleted/empty)

static fs::path rms_path(const std::string& store_name) {
    return fs::path(g_rms_dir) / (store_name + ".rms");
}

// Defensive read. If the file is truncated, has a garbage record count, or
// has a garbage record length, reject the whole load and clear the in-memory
// store so the game starts fresh — far safer than handing bogus state back
// to the MIDlet and watching it self-destruct. Saves from a crashed/killed
// previous run used to manifest as games stuck on boot animation.
static void rms_load(const std::string& store_name) {
    auto path = rms_path(store_name);
    if (!fs::exists(path)) return;
    std::ifstream in(path, std::ios::binary);
    if (!in) return;

    // Hard caps so a corrupt header can't ask us to allocate gigabytes.
    constexpr uint32_t kMaxRecords      = 1u << 20;   // 1M records
    constexpr uint32_t kMaxRecordBytes  = 16u << 20;  // 16 MB per record

    auto fail = [&](const char* why) {
        fprintf(stderr, "[rms] discarding corrupt %s (%s)\n",
                path.c_str(), why);
        g_record_stores[store_name].clear();
        // Leave the file on disk so the user can inspect — the game will
        // overwrite it on the next save via atomic rename.
    };

    uint32_t count = 0;
    if (!in.read(reinterpret_cast<char*>(&count), 4)) return fail("short header");
    if (count > kMaxRecords) return fail("implausible record count");

    auto& recs = g_record_stores[store_name];
    recs.clear();
    recs.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t len = 0;
        if (!in.read(reinterpret_cast<char*>(&len), 4))
            return fail("truncated in record header");
        if (len == 0xFFFFFFFFu) { recs[i].clear(); continue; }
        if (len > kMaxRecordBytes) return fail("implausible record length");
        recs[i].resize(len);
        if (len > 0 && !in.read(reinterpret_cast<char*>(recs[i].data()), len))
            return fail("truncated in record body");
    }
    fprintf(stderr, "[rms] loaded %s: %u records\n", store_name.c_str(), count);
}

// Atomic write: build the file as <name>.rms.tmp, flush+close, then rename
// over the real path. POSIX guarantees the rename is atomic on the same
// filesystem, so readers see either the old file or the new one — never a
// partially-written mix. Before this, Ctrl+C / ESC during a save could
// leave a truncated file that poisoned the next launch.
static void rms_save(const std::string& store_name) {
    if (g_rms_dir.empty()) return;
    fs::create_directories(g_rms_dir);
    auto path = rms_path(store_name);
    auto tmp  = path;
    tmp += ".tmp";

    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            fprintf(stderr, "[rms] failed to open %s\n", tmp.c_str());
            return;
        }
        auto& recs = g_record_stores[store_name];
        uint32_t count = (uint32_t)recs.size();
        out.write(reinterpret_cast<const char*>(&count), 4);
        for (auto& rec : recs) {
            if (rec.empty()) {
                uint32_t marker = 0xFFFFFFFFu;
                out.write(reinterpret_cast<const char*>(&marker), 4);
            } else {
                uint32_t len = (uint32_t)rec.size();
                out.write(reinterpret_cast<const char*>(&len), 4);
                out.write(reinterpret_cast<const char*>(rec.data()), len);
            }
        }
        out.flush();
        if (!out) {
            fprintf(stderr, "[rms] write failed on %s\n", tmp.c_str());
            fs::remove(tmp);
            return;
        }
    }  // ofstream destructor closes the file

    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fprintf(stderr, "[rms] rename %s -> %s failed: %s\n",
                tmp.c_str(), path.c_str(), ec.message().c_str());
        fs::remove(tmp, ec);
    }
}

// ─── Helper: class objects ────────────────────────────────────────────────────

static ObjRef get_class_object(VM& vm, ClassDef* klass) {
    auto it = g_class_objects.find(klass);
    if (it != g_class_objects.end()) return it->second;

    ClassDef* jlClass = vm.loader().find_or_stub("java/lang/Class");
    // Allocate a 2-slot object so we can store the ClassDef pointer split
    // across two int32_t fields.
    ObjRef ref = vm.heap().alloc_object(jlClass, 2);
    if (ref == NULL_REF) return NULL_REF;

    uintptr_t ptr = reinterpret_cast<uintptr_t>(klass);
    HeapObject* obj = vm.heap().deref(ref);
    obj->field(0).raw = static_cast<int32_t>(ptr & 0xFFFFFFFFu);
    obj->field(1).raw = static_cast<int32_t>((ptr >> 32) & 0xFFFFFFFFu);

    g_class_objects[klass] = ref;
    return ref;
}

static ClassDef* class_from_object(VM& vm, ObjRef ref) {
    if (ref == NULL_REF) return nullptr;
    HeapObject* obj = vm.heap().deref(ref);
    if (!obj) return nullptr;
    uint32_t lo = static_cast<uint32_t>(obj->field(0).raw);
    uint32_t hi = static_cast<uint32_t>(obj->field(1).raw);
    uintptr_t ptr = (static_cast<uintptr_t>(hi) << 32) | lo;
    return reinterpret_cast<ClassDef*>(ptr);
}

// ─── Helper: ensure class has at least N slots ────────────────────────────────
// Called before allocating objects of stub classes that we will access fields on.
static void ensure_slots(ClassDef* klass, uint32_t min_slots) {
    if (klass->instance_slot_count < min_slots)
        klass->instance_slot_count = min_slots;
}

// ─── Registration ─────────────────────────────────────────────────────────────

void register_natives(VM& vm, const JarFile& jar) {

    // Set up persistent RMS directory based on JAR name
    {
        fs::path jar_stem = fs::path(jar.path()).stem();
        const char* home = getenv("HOME");
        if (home)
            g_rms_dir = (fs::path(home) / ".j2me" / jar_stem.string() / "rms").string();
        fprintf(stderr, "[rms] storage dir: %s\n", g_rms_dir.c_str());
    }

    // java/lang/Class needs 2 slots to hold a ClassDef* pointer
    ensure_slots(vm.loader().find_or_stub("java/lang/Class"), 2);

    // Pre-initialize java.lang.System static fields so getstatic doesn't get null
    {
        ClassDef* ps_klass = vm.loader().find_or_stub("java/io/PrintStream");
        ObjRef stdout_ref  = vm.heap().alloc_object(ps_klass, 0);
        ObjRef stderr_ref  = vm.heap().alloc_object(ps_klass, 0);
        vm.set_static("java/lang/System", "out", "Ljava/io/PrintStream;",
                      Slot::from_ref(stdout_ref));
        vm.set_static("java/lang/System", "err", "Ljava/io/PrintStream;",
                      Slot::from_ref(stderr_ref));
    }

    // ── java.lang.Throwable ──────────────────────────────────────────────────
    // We don't have a real exception hierarchy; just make these no-ops so game
    // code can call e.printStackTrace() / e.getMessage() without crashing.
    vm.register_native("java/lang/Throwable", "<init>",          "()V",                    [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/lang/Throwable", "<init>",          "(Ljava/lang/String;)V",  [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/lang/Throwable", "printStackTrace", "()V",                    [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/lang/Throwable", "getMessage",      "()Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot>) { f.push_ref(v.new_string("")); });
    vm.register_native("java/lang/Throwable", "toString",        "()Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            ClassDef* k = v.heap().deref(args[0].as_ref()) ?
                          v.heap().deref(args[0].as_ref())->klass : nullptr;
            std::string name = k ? k->name : "java/lang/Throwable";
            std::replace(name.begin(), name.end(), '/', '.');
            f.push_ref(v.new_string(name));
        });
    vm.register_native("java/lang/Exception", "<init>",          "()V",                    [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/lang/Exception", "<init>",          "(Ljava/lang/String;)V",  [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/lang/Exception", "printStackTrace", "()V",                    [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/lang/Exception", "getMessage",      "()Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot>) { f.push_ref(v.new_string("")); });
    vm.register_native("java/lang/RuntimeException", "printStackTrace", "()V",             [](VM&, Frame&, std::span<Slot>) {});

    // ── java.lang.Object ─────────────────────────────────────────────────────

    vm.register_native("java/lang/Object", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot>) {});

    vm.register_native("java/lang/Object", "getClass",
        "()Ljava/lang/Class;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            HeapObject* obj = v.heap().deref(self);
            ClassDef* klass = obj ? obj->klass : nullptr;
            if (!klass) klass = v.loader().find_or_stub("java/lang/Object");
            f.push_ref(get_class_object(v, klass));
        });

    vm.register_native("java/lang/Object", "hashCode", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            f.push_int(static_cast<int32_t>(args[0].as_ref() * 2654435761u));
        });

    vm.register_native("java/lang/Object", "equals",
        "(Ljava/lang/Object;)Z",
        [](VM&, Frame& f, std::span<Slot> args) {
            f.push_int(args[0].as_ref() == args[1].as_ref() ? 1 : 0);
        });

    vm.register_native("java/lang/Object", "toString",
        "()Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            HeapObject* obj = v.heap().deref(self);
            std::string name = obj && obj->klass ? obj->klass->name : "Object";
            f.push_ref(v.new_string(name + "@" + std::to_string(self)));
        });

    // We don't have real monitors. Games often wait() on one thread until a
    // sibling thread calls notify(). With sequential execution that would
    // deadlock, so wait() cooperatively yields — run any pending thread's run()
    // to completion so it has a chance to produce the state the waiter wants.
    vm.register_native("java/lang/Object", "wait", "()V",
        [](VM& v, Frame&, std::span<Slot>) {
            if (!v.run_next_pending_thread())
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
        });
    vm.register_native("java/lang/Object", "wait", "(J)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            Slot2 s2; s2.lo = args[1].raw; s2.hi = args[2].raw;
            int64_t ms = s2.as_long();
            if (!v.run_next_pending_thread()) {
                if (ms <= 0) ms = 16;
                if (ms > 1000) ms = 1000;
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            }
        });
    vm.register_native("java/lang/Object", "notify", "()V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/lang/Object", "notifyAll", "()V",
        [](VM&, Frame&, std::span<Slot>) {});

    // ── java.lang.Class ──────────────────────────────────────────────────────

    vm.register_native("java/lang/Class", "getName",
        "()Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            ClassDef* klass = class_from_object(v, args[0].as_ref());
            std::string name = klass ? klass->name : "unknown";
            std::replace(name.begin(), name.end(), '/', '.');
            f.push_ref(v.new_string(name));
        });

    vm.register_native("java/lang/Class", "forName",
        "(Ljava/lang/String;)Ljava/lang/Class;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            std::string name = v.string_value(args[0].as_ref());
            std::replace(name.begin(), name.end(), '.', '/');
            ClassDef* klass = v.loader().find(name);
            if (!klass || klass->source == nullptr) {
                ClassDef* exk = v.loader().find_or_stub("java/lang/ClassNotFoundException");
                ObjRef ex = v.heap().alloc_object(exk, 0);
                throw JvmException{ex, "ClassNotFoundException: " + name, {}};
            }
            f.push_ref(get_class_object(v, klass));
        });

    vm.register_native("java/lang/Class", "getResourceAsStream",
        "(Ljava/lang/String;)Ljava/io/InputStream;",
        [&jar](VM& v, Frame& f, std::span<Slot> args) {
            std::string requested = v.string_value(args[1].as_ref());
            std::string path = jar.resolve(requested);
            if (path.empty()) {
                fprintf(stderr, "[native] getResourceAsStream: not found: %s\n",
                        requested.c_str());
                f.push_ref(NULL_REF);
                return;
            }

            ClassDef* isClass = v.loader().find_or_stub("java/io/InputStream");
            ObjRef stream = v.heap().alloc_object(isClass, 0);
            g_streams[stream] = StreamEntry{jar.get(path), 0};
            f.push_ref(stream);
        });

    // ── java.io.InputStream / DataInputStream ─────────────────────────────────

    auto stream_read1 = [](VM&, Frame& f, std::span<Slot> args) {
        ObjRef self = args[0].as_ref();
        auto it = g_streams.find(self);
        if (it == g_streams.end() || it->second.pos >= (int32_t)it->second.data.size()) {
            f.push_int(-1); return;
        }
        f.push_int(static_cast<uint8_t>(it->second.data[it->second.pos++]));
    };
    vm.register_native("java/io/InputStream",    "read", "()I", stream_read1);
    vm.register_native("java/io/DataInputStream","read", "()I", stream_read1);

    auto stream_read_buf = [](VM& v, Frame& f, std::span<Slot> args) {
        ObjRef self = args[0].as_ref(), buf = args[1].as_ref();
        auto it = g_streams.find(self);
        if (it == g_streams.end()) { f.push_int(-1); return; }
        StreamEntry& s = it->second;
        HeapObject* arr = v.heap().deref(buf);
        int32_t cap   = arr ? arr->array_length() : 0;
        int32_t avail = static_cast<int32_t>(s.data.size()) - s.pos;
        int32_t n     = std::min(cap, avail);
        if (n <= 0) { f.push_int(-1); return; }
        std::memcpy(arr->array_bytes(), s.data.data() + s.pos, n);
        s.pos += n;
        f.push_int(n);
    };
    vm.register_native("java/io/InputStream",    "read", "([B)I", stream_read_buf);
    vm.register_native("java/io/DataInputStream","read", "([B)I", stream_read_buf);

    auto stream_read_range = [](VM& v, Frame& f, std::span<Slot> args) {
        ObjRef self = args[0].as_ref(), buf = args[1].as_ref();
        int32_t off = args[2].as_int(), len = args[3].as_int();
        auto it = g_streams.find(self);
        if (it == g_streams.end()) { f.push_int(-1); return; }
        StreamEntry& s = it->second;
        HeapObject* arr = v.heap().deref(buf);
        if (!arr) { f.push_int(-1); return; }
        int32_t avail = static_cast<int32_t>(s.data.size()) - s.pos;
        int32_t n = std::min(len, avail);
        if (n <= 0) { f.push_int(-1); return; }
        std::memcpy(arr->array_bytes() + off, s.data.data() + s.pos, n);
        s.pos += n;
        f.push_int(n);
    };
    vm.register_native("java/io/InputStream",    "read", "([BII)I", stream_read_range);
    vm.register_native("java/io/DataInputStream","read", "([BII)I", stream_read_range);

    vm.register_native("java/io/InputStream", "available", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_streams.find(args[0].as_ref());
            if (it == g_streams.end()) { f.push_int(0); return; }
            int32_t r = static_cast<int32_t>(it->second.data.size()) - it->second.pos;
            f.push_int(std::max(0, r));
        });

    vm.register_native("java/io/InputStream", "close", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_streams.erase(args[0].as_ref());
        });
    vm.register_native("java/io/DataInputStream", "close", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_streams.erase(args[0].as_ref());
        });

    vm.register_native("java/io/DataInputStream", "<init>",
        "(Ljava/io/InputStream;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref(), src = args[1].as_ref();
            auto it = g_streams.find(src);
            if (it != g_streams.end()) {
                g_streams[self] = std::move(it->second);
                g_streams.erase(it);
            }
        });

    vm.register_native("java/io/DataInputStream", "readShort", "()S",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_streams.find(args[0].as_ref());
            if (it == g_streams.end() || it->second.pos + 2 > (int32_t)it->second.data.size())
                throw JvmException{NULL_REF, "EOFException"};
            StreamEntry& s = it->second;
            int16_t v = static_cast<int16_t>(
                (uint16_t(s.data[s.pos]) << 8) | s.data[s.pos+1]);
            s.pos += 2;
            f.push_int(v);
        });

    vm.register_native("java/io/DataInputStream", "readByte", "()B",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_streams.find(args[0].as_ref());
            if (it == g_streams.end() || it->second.pos >= (int32_t)it->second.data.size())
                throw JvmException{NULL_REF, "EOFException"};
            f.push_int(static_cast<int8_t>(it->second.data[it->second.pos++]));
        });

    vm.register_native("java/io/DataInputStream", "readInt", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_streams.find(args[0].as_ref());
            if (it == g_streams.end() || it->second.pos + 4 > (int32_t)it->second.data.size())
                throw JvmException{NULL_REF, "EOFException"};
            StreamEntry& s = it->second;
            int32_t v = (int32_t(s.data[s.pos  ]) << 24) |
                        (int32_t(s.data[s.pos+1]) << 16) |
                        (int32_t(s.data[s.pos+2]) <<  8) |
                         int32_t(s.data[s.pos+3]);
            s.pos += 4;
            f.push_int(v);
        });

    vm.register_native("java/io/DataInputStream", "readUTF", "()Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            auto it = g_streams.find(args[0].as_ref());
            if (it == g_streams.end() || it->second.pos + 2 > (int32_t)it->second.data.size())
                throw JvmException{NULL_REF, "EOFException"};
            StreamEntry& s = it->second;
            // Modified UTF-8: first 2 bytes are big-endian length
            uint16_t len = (uint16_t(s.data[s.pos]) << 8) | s.data[s.pos + 1];
            s.pos += 2;
            if (s.pos + len > (int32_t)s.data.size())
                throw JvmException{NULL_REF, "EOFException"};
            // Modified UTF-8 is compatible enough with UTF-8 for most text
            std::string str(reinterpret_cast<const char*>(s.data.data() + s.pos), len);
            s.pos += len;
            f.push_ref(v.new_string(str));
        });

    vm.register_native("java/io/DataInputStream", "readLong", "()J",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_streams.find(args[0].as_ref());
            if (it == g_streams.end() || it->second.pos + 8 > (int32_t)it->second.data.size())
                throw JvmException{NULL_REF, "EOFException"};
            StreamEntry& s = it->second;
            int64_t val = 0;
            for (int i = 0; i < 8; i++)
                val = (val << 8) | s.data[s.pos + i];
            s.pos += 8;
            f.push_long(val);
        });

    vm.register_native("java/io/DataInputStream", "readBoolean", "()Z",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_streams.find(args[0].as_ref());
            if (it == g_streams.end() || it->second.pos >= (int32_t)it->second.data.size())
                throw JvmException{NULL_REF, "EOFException"};
            f.push_int(it->second.data[it->second.pos++] != 0 ? 1 : 0);
        });

    vm.register_native("java/io/DataInputStream", "readUnsignedByte", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_streams.find(args[0].as_ref());
            if (it == g_streams.end() || it->second.pos >= (int32_t)it->second.data.size())
                throw JvmException{NULL_REF, "EOFException"};
            f.push_int(static_cast<uint8_t>(it->second.data[it->second.pos++]));
        });

    vm.register_native("java/io/DataInputStream", "readUnsignedShort", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_streams.find(args[0].as_ref());
            if (it == g_streams.end() || it->second.pos + 2 > (int32_t)it->second.data.size())
                throw JvmException{NULL_REF, "EOFException"};
            StreamEntry& s = it->second;
            uint16_t val = (uint16_t(s.data[s.pos]) << 8) | s.data[s.pos + 1];
            s.pos += 2;
            f.push_int(val);
        });

    vm.register_native("java/io/DataInputStream", "readFully", "([B)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            auto it = g_streams.find(args[0].as_ref());
            ObjRef buf = args[1].as_ref();
            HeapObject* arr = v.heap().deref(buf);
            if (!it->second.data.size() || !arr) return;
            StreamEntry& s = it->second;
            int32_t len = arr->array_length();
            if (s.pos + len > (int32_t)s.data.size())
                throw JvmException{NULL_REF, "EOFException"};
            std::memcpy(arr->array_bytes(), s.data.data() + s.pos, len);
            s.pos += len;
        });

    vm.register_native("java/io/DataInputStream", "readFully", "([BII)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            auto it = g_streams.find(args[0].as_ref());
            ObjRef buf = args[1].as_ref();
            int32_t off = args[2].as_int(), len = args[3].as_int();
            HeapObject* arr = v.heap().deref(buf);
            if (!arr) return;
            StreamEntry& s = it->second;
            if (s.pos + len > (int32_t)s.data.size())
                throw JvmException{NULL_REF, "EOFException"};
            std::memcpy(arr->array_bytes() + off, s.data.data() + s.pos, len);
            s.pos += len;
        });

    // skip() and reset() — shared across InputStream, DataInputStream, ByteArrayInputStream
    auto stream_skip = [](VM&, Frame& f, std::span<Slot> args) {
        auto it = g_streams.find(args[0].as_ref());
        if (it == g_streams.end()) { f.push_long(0); return; }
        StreamEntry& s = it->second;
        // args[1],args[2] = long (lo,hi)
        Slot2 s2; s2.lo = args[1].raw; s2.hi = args[2].raw;
        int64_t n = s2.as_long();
        int64_t avail = (int64_t)s.data.size() - s.pos;
        int64_t skipped = std::min(n, avail);
        if (skipped < 0) skipped = 0;
        s.pos += (int32_t)skipped;
        f.push_long(skipped);
    };

    vm.register_native("java/io/InputStream",         "skip", "(J)J", stream_skip);
    vm.register_native("java/io/DataInputStream",     "skip", "(J)J", stream_skip);

    // DataInputStream.skipBytes(I)I — returns int, not long
    vm.register_native("java/io/DataInputStream", "skipBytes", "(I)I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_streams.find(args[0].as_ref());
            if (it == g_streams.end()) { f.push_int(0); return; }
            StreamEntry& s = it->second;
            int32_t n = args[1].as_int();
            int32_t avail = (int32_t)s.data.size() - s.pos;
            int32_t skipped = std::min(n, avail);
            if (skipped < 0) skipped = 0;
            s.pos += skipped;
            f.push_int(skipped);
        });

    auto stream_mark = [](VM&, Frame&, std::span<Slot> args) {
        auto it = g_streams.find(args[0].as_ref());
        if (it != g_streams.end()) it->second.mark = it->second.pos;
    };
    vm.register_native("java/io/InputStream",         "mark", "(I)V", stream_mark);
    vm.register_native("java/io/DataInputStream",     "mark", "(I)V", stream_mark);

    auto stream_reset = [](VM&, Frame&, std::span<Slot> args) {
        auto it = g_streams.find(args[0].as_ref());
        if (it != g_streams.end()) it->second.pos = it->second.mark;
    };
    vm.register_native("java/io/InputStream",         "reset", "()V", stream_reset);
    vm.register_native("java/io/DataInputStream",     "reset", "()V", stream_reset);

    vm.register_native("java/io/InputStream",         "markSupported", "()Z",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(1); });
    vm.register_native("java/io/DataInputStream",     "markSupported", "()Z",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(1); });

    // ── java.io.ByteArrayInputStream ─────────────────────────────────────────
    // MIDP games frequently wrap decoded resource data in a ByteArrayInputStream.

    auto bais_init = [](VM& v, Frame&, std::span<Slot> args) {
        ObjRef self     = args[0].as_ref();
        ObjRef arr_ref  = args[1].as_ref();
        HeapObject* arr = v.heap().deref(arr_ref);
        if (!arr) return;
        int32_t offset = 0, count = arr->array_length();
        if (args.size() >= 4) { offset = args[2].as_int(); count = args[3].as_int(); }
        StreamEntry se;
        uint8_t* bytes = reinterpret_cast<uint8_t*>(arr->array_bytes());
        se.data.assign(bytes + offset, bytes + offset + count);
        se.pos = 0;
        g_streams[self] = std::move(se);
    };
    vm.register_native("java/io/ByteArrayInputStream", "<init>", "([B)V",   bais_init);
    vm.register_native("java/io/ByteArrayInputStream", "<init>", "([BII)V", bais_init);
    vm.register_native("java/io/ByteArrayInputStream", "read",   "()I",     stream_read1);
    vm.register_native("java/io/ByteArrayInputStream", "read",   "([B)I",   stream_read_buf);
    vm.register_native("java/io/ByteArrayInputStream", "read",   "([BII)I", stream_read_range);
    vm.register_native("java/io/ByteArrayInputStream", "available", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_streams.find(args[0].as_ref());
            if (it == g_streams.end()) { f.push_int(0); return; }
            f.push_int(std::max(0, (int32_t)it->second.data.size() - it->second.pos));
        });
    vm.register_native("java/io/ByteArrayInputStream", "close", "()V",
        [](VM&, Frame&, std::span<Slot> args) { g_streams.erase(args[0].as_ref()); });
    vm.register_native("java/io/ByteArrayInputStream", "skip",  "(J)J", stream_skip);
    vm.register_native("java/io/ByteArrayInputStream", "mark",  "(I)V", stream_mark);
    vm.register_native("java/io/ByteArrayInputStream", "reset", "()V",  stream_reset);
    vm.register_native("java/io/ByteArrayInputStream", "markSupported", "()Z",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(1); });

    // ── java.io.ByteArrayOutputStream ─────────────────────────────────────────
    // Keyed the same way as input streams — writes accumulate into
    // g_streams[self].data; toByteArray() materialises the buffer.
    vm.register_native("java/io/ByteArrayOutputStream", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_streams[args[0].as_ref()] = StreamEntry{};
        });
    vm.register_native("java/io/ByteArrayOutputStream", "<init>", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            StreamEntry se;
            se.data.reserve((size_t)std::max(0, args[1].as_int()));
            g_streams[args[0].as_ref()] = std::move(se);
        });
    vm.register_native("java/io/ByteArrayOutputStream", "write", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto& s = g_streams[args[0].as_ref()];
            s.data.push_back((uint8_t)(args[1].as_int() & 0xFF));
        });
    vm.register_native("java/io/ByteArrayOutputStream", "write", "([BII)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            auto& s  = g_streams[args[0].as_ref()];
            HeapObject* arr = v.heap().deref(args[1].as_ref());
            if (!arr) return;
            int32_t off = args[2].as_int(), len = args[3].as_int();
            int32_t alen = arr->array_length();
            if (off < 0 || len < 0 || off + len > alen) return;
            uint8_t* bytes = reinterpret_cast<uint8_t*>(arr->array_bytes());
            s.data.insert(s.data.end(), bytes + off, bytes + off + len);
        });
    vm.register_native("java/io/ByteArrayOutputStream", "size", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_streams.find(args[0].as_ref());
            f.push_int(it == g_streams.end() ? 0 : (int32_t)it->second.data.size());
        });
    vm.register_native("java/io/ByteArrayOutputStream", "reset", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto it = g_streams.find(args[0].as_ref());
            if (it != g_streams.end()) it->second.data.clear();
        });
    vm.register_native("java/io/ByteArrayOutputStream", "close", "()V",
        [](VM&, Frame&, std::span<Slot>) {});  // games often reuse after close
    vm.register_native("java/io/ByteArrayOutputStream", "flush", "()V",
        [](VM&, Frame&, std::span<Slot>) {});
    // ── java.io.DataOutputStream ──────────────────────────────────────────────
    // Wraps another OutputStream. We track the underlying stream ref and
    // forward writes to its entry in g_streams.
    static std::unordered_map<ObjRef, ObjRef> g_dos_wraps;  // dos -> underlying

    auto dos_target = [](ObjRef dos) -> StreamEntry* {
        auto it = g_dos_wraps.find(dos);
        if (it == g_dos_wraps.end()) return nullptr;
        auto sit = g_streams.find(it->second);
        return sit == g_streams.end() ? nullptr : &sit->second;
    };

    vm.register_native("java/io/DataOutputStream", "<init>",
        "(Ljava/io/OutputStream;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_dos_wraps[args[0].as_ref()] = args[1].as_ref();
        });
    vm.register_native("java/io/DataOutputStream", "write", "(I)V",
        [dos_target](VM&, Frame&, std::span<Slot> args) {
            if (auto* s = dos_target(args[0].as_ref()))
                s->data.push_back((uint8_t)(args[1].as_int() & 0xFF));
        });
    vm.register_native("java/io/DataOutputStream", "writeByte", "(I)V",
        [dos_target](VM&, Frame&, std::span<Slot> args) {
            if (auto* s = dos_target(args[0].as_ref()))
                s->data.push_back((uint8_t)(args[1].as_int() & 0xFF));
        });
    vm.register_native("java/io/DataOutputStream", "writeBoolean", "(Z)V",
        [dos_target](VM&, Frame&, std::span<Slot> args) {
            if (auto* s = dos_target(args[0].as_ref()))
                s->data.push_back(args[1].as_int() ? 1 : 0);
        });
    vm.register_native("java/io/DataOutputStream", "writeShort", "(I)V",
        [dos_target](VM&, Frame&, std::span<Slot> args) {
            if (auto* s = dos_target(args[0].as_ref())) {
                int32_t v = args[1].as_int();
                s->data.push_back((uint8_t)((v >> 8) & 0xFF));
                s->data.push_back((uint8_t)(v & 0xFF));
            }
        });
    vm.register_native("java/io/DataOutputStream", "writeChar", "(I)V",
        [dos_target](VM&, Frame&, std::span<Slot> args) {
            if (auto* s = dos_target(args[0].as_ref())) {
                int32_t v = args[1].as_int();
                s->data.push_back((uint8_t)((v >> 8) & 0xFF));
                s->data.push_back((uint8_t)(v & 0xFF));
            }
        });
    vm.register_native("java/io/DataOutputStream", "writeInt", "(I)V",
        [dos_target](VM&, Frame&, std::span<Slot> args) {
            if (auto* s = dos_target(args[0].as_ref())) {
                int32_t v = args[1].as_int();
                s->data.push_back((uint8_t)((v >> 24) & 0xFF));
                s->data.push_back((uint8_t)((v >> 16) & 0xFF));
                s->data.push_back((uint8_t)((v >> 8)  & 0xFF));
                s->data.push_back((uint8_t)(v & 0xFF));
            }
        });
    vm.register_native("java/io/DataOutputStream", "writeLong", "(J)V",
        [dos_target](VM&, Frame&, std::span<Slot> args) {
            if (auto* s = dos_target(args[0].as_ref())) {
                Slot2 s2; s2.lo = args[1].raw; s2.hi = args[2].raw;
                int64_t v = s2.as_long();
                for (int i = 7; i >= 0; --i)
                    s->data.push_back((uint8_t)((v >> (i * 8)) & 0xFF));
            }
        });
    vm.register_native("java/io/DataOutputStream", "writeUTF",
        "(Ljava/lang/String;)V",
        [dos_target](VM& v, Frame&, std::span<Slot> args) {
            auto* s = dos_target(args[0].as_ref());
            if (!s) return;
            std::string str = v.string_value(args[1].as_ref());
            // Modified UTF-8 (short BE length prefix + raw bytes; good enough
            // for ASCII which is all J2ME games typically store).
            uint16_t len = (uint16_t)std::min(str.size(), (size_t)0xFFFF);
            s->data.push_back((uint8_t)(len >> 8));
            s->data.push_back((uint8_t)(len & 0xFF));
            s->data.insert(s->data.end(), str.begin(), str.begin() + len);
        });
    vm.register_native("java/io/DataOutputStream", "write", "([BII)V",
        [dos_target](VM& v, Frame&, std::span<Slot> args) {
            auto* s = dos_target(args[0].as_ref());
            if (!s) return;
            HeapObject* arr = v.heap().deref(args[1].as_ref());
            if (!arr) return;
            int32_t off = args[2].as_int(), len = args[3].as_int();
            int32_t alen = arr->array_length();
            if (off < 0 || len < 0 || off + len > alen) return;
            uint8_t* bytes = reinterpret_cast<uint8_t*>(arr->array_bytes());
            s->data.insert(s->data.end(), bytes + off, bytes + off + len);
        });
    vm.register_native("java/io/DataOutputStream", "flush", "()V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/io/DataOutputStream", "close", "()V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/io/DataOutputStream", "size", "()I",
        [dos_target](VM&, Frame& f, std::span<Slot> args) {
            auto* s = dos_target(args[0].as_ref());
            f.push_int(s ? (int32_t)s->data.size() : 0);
        });

    vm.register_native("java/io/ByteArrayOutputStream", "toByteArray", "()[B",
        [](VM& v, Frame& f, std::span<Slot> args) {
            auto it = g_streams.find(args[0].as_ref());
            size_t n = (it == g_streams.end()) ? 0 : it->second.data.size();
            ObjRef arr = v.heap().alloc_prim_array(
                ArrayType::Byte, (int32_t)n, v.loader().find_or_stub("[B"));
            if (arr != NULL_REF && n > 0) {
                HeapObject* ao = v.heap().deref(arr);
                std::memcpy(ao->array_bytes(), it->second.data.data(), n);
            }
            f.push_ref(arr);
        });

    // ── java.lang.String ─────────────────────────────────────────────────────

    vm.register_native("java/lang/String", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/lang/String", "<init>", "(Ljava/lang/String;)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref(), src = args[1].as_ref();
            if (src == NULL_REF) return;
            HeapObject* sobj = v.heap().deref(src);
            HeapObject* dobj = v.heap().deref(self);
            if (sobj && dobj && sobj->data_words >= 2 && dobj->data_words >= 2) {
                dobj->field(0) = sobj->field(0);  // char[] ref
                dobj->field(1) = sobj->field(1);  // count
            }
        });

    // String from char array: chars[off..off+count)
    auto string_init_chars = [](VM& v, Frame&, std::span<Slot> args,
                                int32_t off, int32_t count) {
        ObjRef self = args[0].as_ref();
        ObjRef src_arr = args[1].as_ref();
        HeapObject* dobj = v.heap().deref(self);
        if (!dobj || dobj->data_words < 2) return;
        if (src_arr == NULL_REF || count < 0) {
            dobj->field(0) = Slot::from_ref(NULL_REF);
            dobj->field(1) = Slot::from_int(0);
            return;
        }
        HeapObject* src = v.heap().deref(src_arr);
        int32_t src_len = src ? src->array_length() : 0;
        if (off < 0 || off > src_len || off + count > src_len) {
            off = 0;
            if (count > src_len) count = src_len;
        }
        ObjRef new_arr = v.heap().alloc_prim_array(
            ArrayType::Char, count, v.loader().find_or_stub("[C"));
        HeapObject* narr = v.heap().deref(new_arr);
        uint16_t* dst = narr->array_shorts();
        uint16_t* ss  = src->array_shorts();
        for (int32_t i = 0; i < count; ++i) dst[i] = ss[off + i];
        dobj->field(0) = Slot::from_ref(new_arr);
        dobj->field(1) = Slot::from_int(count);
        if (std::getenv("J2ME_TRACE_CHAR")) {
            fprintf(stderr, "[char] String.<init>([CII) len=%d \"", count);
            for (int32_t i = 0; i < count && i < 200; ++i) {
                uint16_t c = ss[off + i];
                if (c >= 0x20 && c < 0x7F) fputc((char)c, stderr);
                else fprintf(stderr, "<%x>", c);
            }
            fprintf(stderr, "\"\n");
        }
    };
    vm.register_native("java/lang/String", "<init>", "([C)V",
        [string_init_chars](VM& v, Frame& f, std::span<Slot> args) {
            HeapObject* src = v.heap().deref(args[1].as_ref());
            int32_t len = src ? src->array_length() : 0;
            string_init_chars(v, f, args, 0, len);
        });

    // String from byte array — treat bytes as ISO-8859-1 (CLDC default).
    auto string_init_bytes = [](VM& v, Frame&, std::span<Slot> args,
                                int32_t off, int32_t count) {
        ObjRef self = args[0].as_ref();
        ObjRef src_arr = args[1].as_ref();
        HeapObject* dobj = v.heap().deref(self);
        if (!dobj || dobj->data_words < 2) return;
        HeapObject* src = (src_arr != NULL_REF) ? v.heap().deref(src_arr) : nullptr;
        int32_t src_len = src ? src->array_length() : 0;
        if (off < 0) off = 0;
        if (count < 0) count = 0;
        if (off > src_len) { count = 0; off = 0; }
        else if (off + count > src_len) count = src_len - off;
        ObjRef new_arr = v.heap().alloc_prim_array(
            ArrayType::Char, count, v.loader().find_or_stub("[C"));
        HeapObject* narr = v.heap().deref(new_arr);
        uint16_t* dst = narr->array_shorts();
        if (src && count > 0) {
            const uint8_t* ss = src->array_bytes();
            for (int32_t i = 0; i < count; ++i) dst[i] = ss[off + i];
        }
        dobj->field(0) = Slot::from_ref(new_arr);
        dobj->field(1) = Slot::from_int(count);
    };
    vm.register_native("java/lang/String", "<init>", "([B)V",
        [string_init_bytes](VM& v, Frame& f, std::span<Slot> args) {
            HeapObject* src = v.heap().deref(args[1].as_ref());
            int32_t len = src ? src->array_length() : 0;
            string_init_bytes(v, f, args, 0, len);
        });
    vm.register_native("java/lang/String", "<init>", "([BII)V",
        [string_init_bytes](VM& v, Frame& f, std::span<Slot> args) {
            string_init_bytes(v, f, args, args[2].as_int(), args[3].as_int());
        });
    vm.register_native("java/lang/String", "<init>", "([BLjava/lang/String;)V",
        [string_init_bytes](VM& v, Frame& f, std::span<Slot> args) {
            // Ignore the encoding argument — treat as Latin-1.
            HeapObject* src = v.heap().deref(args[1].as_ref());
            int32_t len = src ? src->array_length() : 0;
            string_init_bytes(v, f, args, 0, len);
        });
    vm.register_native("java/lang/String", "<init>", "([BIILjava/lang/String;)V",
        [string_init_bytes](VM& v, Frame& f, std::span<Slot> args) {
            string_init_bytes(v, f, args, args[2].as_int(), args[3].as_int());
        });
    vm.register_native("java/lang/String", "<init>", "([CII)V",
        [string_init_chars](VM& v, Frame& f, std::span<Slot> args) {
            string_init_chars(v, f, args, args[2].as_int(), args[3].as_int());
        });

    vm.register_native("java/lang/String", "length", "()I",
        [](VM& v, Frame& f, std::span<Slot> args) {
            f.push_int(utf8_char_count(v.string_value(args[0].as_ref())));
        });

    vm.register_native("java/lang/String", "charAt", "(I)C",
        [](VM& v, Frame& f, std::span<Slot> args) {
            std::string s = v.string_value(args[0].as_ref());
            int32_t idx = args[1].as_int();
            size_t byte_off = utf8_char_to_byte(s, idx);
            f.push_int(byte_off < s.size() ? utf8_decode_at(s, byte_off) : 0);
        });

    vm.register_native("java/lang/String", "equals",
        "(Ljava/lang/Object;)Z",
        [](VM& v, Frame& f, std::span<Slot> args) {
            f.push_int(v.string_value(args[0].as_ref()) ==
                       v.string_value(args[1].as_ref()) ? 1 : 0);
        });

    vm.register_native("java/lang/String", "valueOf", "(I)Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            f.push_ref(v.new_string(std::to_string(args[0].as_int())));
        });

    vm.register_native("java/lang/String", "valueOf",
        "(Ljava/lang/Object;)Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            f.push_ref(v.new_string(v.string_value(args[0].as_ref())));
        });

    vm.register_native("java/lang/String", "valueOf", "(C)Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            std::string s;
            s += static_cast<char>(args[0].as_int() & 0xFF);
            f.push_ref(v.new_string(s));
        });
    vm.register_native("java/lang/String", "valueOf", "(Z)Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            f.push_ref(v.new_string(args[0].as_int() ? "true" : "false"));
        });
    vm.register_native("java/lang/String", "valueOf", "(J)Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            int64_t lo = (uint32_t)args[0].as_int();
            int64_t hi = args[1].as_int();
            f.push_ref(v.new_string(std::to_string((hi << 32) | lo)));
        });
vm.register_native("java/lang/String", "valueOf", "([C)Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            HeapObject* arr = v.heap().deref(args[0].as_ref());
            std::string s;
            if (arr) {
                int32_t len = arr->array_length();
                uint16_t* chars = arr->array_shorts();
                for (int32_t i = 0; i < len; ++i)
                    s += static_cast<char>(chars[i] & 0xFF);
            }
            f.push_ref(v.new_string(s));
        });
    vm.register_native("java/lang/String", "valueOf", "([CII)Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            HeapObject* arr = v.heap().deref(args[0].as_ref());
            int32_t off = args[1].as_int(), count = args[2].as_int();
            std::string s;
            if (arr) {
                int32_t len = arr->array_length();
                if (off < 0) off = 0;
                if (off + count > len) count = len - off;
                uint16_t* chars = arr->array_shorts();
                for (int32_t i = 0; i < count; ++i)
                    s += static_cast<char>(chars[off + i] & 0xFF);
            }
            f.push_ref(v.new_string(s));
        });

    vm.register_native("java/lang/String", "indexOf", "(I)I",
        [](VM& v, Frame& f, std::span<Slot> args) {
            std::string s = v.string_value(args[0].as_ref());
            int32_t ch = args[1].as_int();
            // Search character by character
            size_t off = 0;
            int32_t ci = 0;
            while (off < s.size()) {
                size_t start = off;
                uint16_t cp = utf8_decode_at(s, off);
                (void)start;
                if (cp == (uint16_t)ch) { f.push_int(ci); return; }
                ++ci;
            }
            f.push_int(-1);
        });

    vm.register_native("java/lang/String", "indexOf", "(II)I",
        [](VM& v, Frame& f, std::span<Slot> args) {
            std::string s = v.string_value(args[0].as_ref());
            int32_t ch = args[1].as_int();
            int32_t fromIndex = args[2].as_int();
            size_t off = utf8_char_to_byte(s, fromIndex);
            int32_t ci = fromIndex;
            while (off < s.size()) {
                uint16_t cp = utf8_decode_at(s, off);
                if (cp == (uint16_t)ch) { f.push_int(ci); return; }
                ++ci;
            }
            f.push_int(-1);
        });

    vm.register_native("java/lang/String", "indexOf", "(Ljava/lang/String;I)I",
        [](VM& v, Frame& f, std::span<Slot> args) {
            std::string s = v.string_value(args[0].as_ref());
            std::string p = v.string_value(args[1].as_ref());
            int32_t fromIndex = args[2].as_int();
            size_t byte_from = utf8_char_to_byte(s, fromIndex);
            auto byte_pos = s.find(p, byte_from);
            if (byte_pos == std::string::npos) { f.push_int(-1); return; }
            f.push_int(utf8_char_count(s.substr(0, byte_pos)));
        });

    vm.register_native("java/lang/String", "lastIndexOf", "(I)I",
        [](VM& v, Frame& f, std::span<Slot> args) {
            std::string s = v.string_value(args[0].as_ref());
            int32_t ch = args[1].as_int();
            int32_t last = -1;
            size_t off = 0;
            int32_t ci = 0;
            while (off < s.size()) {
                uint16_t cp = utf8_decode_at(s, off);
                if (cp == (uint16_t)ch) last = ci;
                ++ci;
            }
            f.push_int(last);
        });

    vm.register_native("java/lang/String", "substring", "(II)Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            std::string s = v.string_value(args[0].as_ref());
            int32_t from = args[1].as_int(), to = args[2].as_int();
            f.push_ref(v.new_string(utf8_substring(s, from, to)));
        });

    vm.register_native("java/lang/String", "substring", "(I)Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            std::string s = v.string_value(args[0].as_ref());
            int32_t from = args[1].as_int();
            size_t byte_from = utf8_char_to_byte(s, from);
            f.push_ref(v.new_string(s.substr(byte_from)));
        });

    vm.register_native("java/lang/String", "startsWith",
        "(Ljava/lang/String;)Z",
        [](VM& v, Frame& f, std::span<Slot> args) {
            std::string s = v.string_value(args[0].as_ref());
            std::string p = v.string_value(args[1].as_ref());
            f.push_int(s.substr(0, p.size()) == p ? 1 : 0);
        });

    vm.register_native("java/lang/String", "endsWith",
        "(Ljava/lang/String;)Z",
        [](VM& v, Frame& f, std::span<Slot> args) {
            std::string s = v.string_value(args[0].as_ref());
            std::string p = v.string_value(args[1].as_ref());
            bool r = s.size() >= p.size() &&
                     s.compare(s.size()-p.size(), p.size(), p) == 0;
            f.push_int(r ? 1 : 0);
        });

    vm.register_native("java/lang/String", "trim", "()Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            std::string s = v.string_value(args[0].as_ref());
            size_t l = s.find_first_not_of(" \t\r\n");
            size_t r = s.find_last_not_of(" \t\r\n");
            f.push_ref(v.new_string(l == std::string::npos ? "" : s.substr(l, r-l+1)));
        });

    vm.register_native("java/lang/String", "toLowerCase",
        "()Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            std::string s = v.string_value(args[0].as_ref());
            std::transform(s.begin(), s.end(), s.begin(), ::tolower);
            f.push_ref(v.new_string(s));
        });

    vm.register_native("java/lang/String", "toUpperCase",
        "()Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            std::string s = v.string_value(args[0].as_ref());
            std::transform(s.begin(), s.end(), s.begin(), ::toupper);
            f.push_ref(v.new_string(s));
        });

    vm.register_native("java/lang/String", "indexOf",
        "(Ljava/lang/String;)I",
        [](VM& v, Frame& f, std::span<Slot> args) {
            std::string s = v.string_value(args[0].as_ref());
            std::string p = v.string_value(args[1].as_ref());
            auto byte_pos = s.find(p);
            if (byte_pos == std::string::npos) { f.push_int(-1); return; }
            // Convert byte offset to char index
            f.push_int(utf8_char_count(s.substr(0, byte_pos)));
        });

    vm.register_native("java/lang/String", "replace",
        "(CC)Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            std::string s = v.string_value(args[0].as_ref());
            char from = static_cast<char>(args[1].as_int());
            char to   = static_cast<char>(args[2].as_int());
            std::replace(s.begin(), s.end(), from, to);
            f.push_ref(v.new_string(s));
        });

    vm.register_native("java/lang/String", "concat",
        "(Ljava/lang/String;)Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            f.push_ref(v.new_string(v.string_value(args[0].as_ref()) +
                                    v.string_value(args[1].as_ref())));
        });

    vm.register_native("java/lang/String", "compareTo",
        "(Ljava/lang/String;)I",
        [](VM& v, Frame& f, std::span<Slot> args) {
            f.push_int(v.string_value(args[0].as_ref())
                        .compare(v.string_value(args[1].as_ref())));
        });

    vm.register_native("java/lang/String", "equalsIgnoreCase",
        "(Ljava/lang/String;)Z",
        [](VM& v, Frame& f, std::span<Slot> args) {
            std::string a = v.string_value(args[0].as_ref());
            std::string b = v.string_value(args[1].as_ref());
            std::transform(a.begin(), a.end(), a.begin(), ::tolower);
            std::transform(b.begin(), b.end(), b.begin(), ::tolower);
            f.push_int(a == b ? 1 : 0);
        });

    vm.register_native("java/lang/System", "getProperty",
        "(Ljava/lang/String;)Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            std::string key = v.string_value(args[0].as_ref());
            // Return sensible values for common J2ME properties
            std::string val;
            if (key == "microedition.platform") val = "j2me-runtime";
            else if (key == "microedition.encoding") val = "UTF-8";
            else if (key == "microedition.configuration") val = "CLDC-1.1";
            else if (key == "microedition.profiles") val = "MIDP-2.0";
            else if (key == "microedition.locale") val = "en-US";
            else if (key == "user.language") val = "en";
            else if (key == "user.country") val = "US";
            else if (key == "file.separator") val = "/";
            f.push_ref(val.empty() ? NULL_REF : v.new_string(val));
        });

    vm.register_native("java/lang/String", "getChars", "(II[CI)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            std::string s    = v.string_value(args[0].as_ref());
            int32_t srcBegin = args[1].as_int();
            int32_t srcEnd   = args[2].as_int();
            ObjRef dst       = args[3].as_ref();
            int32_t dstBegin = args[4].as_int();
            HeapObject* arr  = v.heap().deref(dst);
            if (!arr) return;
            size_t off = utf8_char_to_byte(s, srcBegin);
            for (int32_t i = 0; i < srcEnd - srcBegin && off < s.size(); ++i)
                arr->array_shorts()[dstBegin + i] = utf8_decode_at(s, off);
        });

    vm.register_native("java/lang/String", "toCharArray", "()[C",
        [](VM& v, Frame& f, std::span<Slot> args) {
            // new_string stores UTF-8 bytes one-per-char in the String's
            // internal char[]. For ASCII text that's identical to UTF-16, but
            // games may also modify the char[] in place (e.g. Spore inserts
            // 0xB6 wrap markers via new String(char[])). We pick the right
            // path by checking whether reserializing + decoding would round-
            // trip losslessly: if ANY char in the array is > 0x7F, the stored
            // representation is not ASCII-as-bytes, so fall back to UTF-8
            // decoding of the serialized bytes (handles Chinese/Latin-1
            // strings loaded via readUTF). Otherwise, directly copy — this
            // preserves game-inserted high chars like 0xB6.
            ObjRef self = args[0].as_ref();
            HeapObject* sobj = v.heap().deref(self);
            if (!sobj || sobj->data_words < 2) {
                ObjRef arr = v.heap().alloc_prim_array(ArrayType::Char, 0,
                                 v.loader().find_or_stub("[C"));
                f.push_ref(arr);
                return;
            }
            ObjRef src_arr = sobj->field(0).as_ref();
            int32_t src_len = sobj->field(1).as_int();
            if (src_len < 0) src_len = 0;
            HeapObject* src = (src_arr != NULL_REF) ? v.heap().deref(src_arr) : nullptr;
            uint16_t* schars = src ? src->array_shorts() : nullptr;

            // Does the char[] look like a valid UTF-8 byte stream? If yes,
            // it came from new_string(utf8) and we need to decode properly.
            // If it contains arbitrary high chars (like 0xB6 wrap markers a
            // game inserted via new String(char[])), direct-copy preserves
            // them instead of mis-merging with neighbors.
            bool looks_utf8 = true;
            if (schars) {
                for (int32_t i = 0; i < src_len; ) {
                    uint16_t c = schars[i];
                    if (c < 0x80) { i += 1; }
                    else if (c < 0xC2) { looks_utf8 = false; break; }
                    else if (c < 0xE0) {
                        if (i + 1 >= src_len ||
                            schars[i+1] < 0x80 || schars[i+1] >= 0xC0)
                            { looks_utf8 = false; break; }
                        i += 2;
                    } else if (c < 0xF0) {
                        if (i + 2 >= src_len ||
                            schars[i+1] < 0x80 || schars[i+1] >= 0xC0 ||
                            schars[i+2] < 0x80 || schars[i+2] >= 0xC0)
                            { looks_utf8 = false; break; }
                        i += 3;
                    } else { looks_utf8 = false; break; }
                }
            }

            if (!looks_utf8) {
                // Direct copy — each char is its own UTF-16 code unit.
                ObjRef arr = v.heap().alloc_prim_array(ArrayType::Char,
                                 src_len, v.loader().find_or_stub("[C"));
                HeapObject* obj = v.heap().deref(arr);
                if (schars && src_len > 0)
                    std::memcpy(obj->array_shorts(), schars,
                                src_len * sizeof(uint16_t));
                f.push_ref(arr);
                return;
            }

            // Valid UTF-8 bytes stored one-per-char — decode into code points.
            std::string s = v.string_value(self);
            int32_t char_len = utf8_char_count(s);
            ObjRef arr = v.heap().alloc_prim_array(ArrayType::Char,
                             char_len, v.loader().find_or_stub("[C"));
            HeapObject* obj = v.heap().deref(arr);
            size_t off = 0;
            for (int32_t i = 0; i < char_len && off < s.size(); ++i)
                obj->array_shorts()[i] = utf8_decode_at(s, off);
            f.push_ref(arr);
        });

    vm.register_native("java/lang/String", "getBytes", "()[B",
        [](VM& v, Frame& f, std::span<Slot> args) {
            std::string s = v.string_value(args[0].as_ref());
            ObjRef arr = v.heap().alloc_prim_array(ArrayType::Byte,
                             static_cast<int32_t>(s.size()),
                             v.loader().find_or_stub("[B"));
            HeapObject* obj = v.heap().deref(arr);
            std::memcpy(obj->array_bytes(), s.data(), s.size());
            f.push_ref(arr);
        });

    vm.register_native("java/lang/String", "hashCode", "()I",
        [](VM& v, Frame& f, std::span<Slot> args) {
            // Java's standard hashCode: h = 31*h + c for each UTF-16 char
            std::string s = v.string_value(args[0].as_ref());
            int32_t h = 0;
            size_t off = 0;
            while (off < s.size())
                h = 31 * h + static_cast<int32_t>(utf8_decode_at(s, off));
            f.push_int(h);
        });

    // ── java.lang.StringBuffer ────────────────────────────────────────────────
    // Backed entirely by g_string_buffers — no heap fields used.

    vm.register_native("java/lang/StringBuffer", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_string_buffers[args[0].as_ref()] = "";
        });

    vm.register_native("java/lang/StringBuffer", "<init>",
        "(Ljava/lang/String;)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            g_string_buffers[args[0].as_ref()] = v.string_value(args[1].as_ref());
        });

    vm.register_native("java/lang/StringBuffer", "<init>", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_string_buffers[args[0].as_ref()] = "";  // capacity hint, ignore
        });

    vm.register_native("java/lang/StringBuffer", "append",
        "(Ljava/lang/String;)Ljava/lang/StringBuffer;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            g_string_buffers[self] += v.string_value(args[1].as_ref());
            f.push_ref(self);
        });

    vm.register_native("java/lang/StringBuffer", "append",
        "(I)Ljava/lang/StringBuffer;",
        [](VM&, Frame& f, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            g_string_buffers[self] += std::to_string(args[1].as_int());
            f.push_ref(self);
        });

    vm.register_native("java/lang/StringBuffer", "append",
        "(C)Ljava/lang/StringBuffer;",
        [](VM&, Frame& f, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            g_string_buffers[self] += static_cast<char>(args[1].as_int() & 0xFF);
            f.push_ref(self);
        });

    vm.register_native("java/lang/StringBuffer", "append",
        "(Ljava/lang/Object;)Ljava/lang/StringBuffer;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            g_string_buffers[self] += v.string_value(args[1].as_ref());
            f.push_ref(self);
        });

    vm.register_native("java/lang/StringBuffer", "toString",
        "()Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            auto it = g_string_buffers.find(args[0].as_ref());
            const std::string& str = it != g_string_buffers.end() ? it->second : "";
            if (std::getenv("J2ME_TRACE_CHAR")) {
                fprintf(stderr, "[char] StringBuffer.toString() len=%zu bytes=\"", str.size());
                for (size_t i = 0; i < str.size() && i < 200; ++i) {
                    uint8_t c = str[i];
                    if (c >= 0x20 && c < 0x7F) fputc((char)c, stderr);
                    else fprintf(stderr, "<%02x>", c);
                }
                fprintf(stderr, "\"\n");
            }
            f.push_ref(v.new_string(str));
        });

    vm.register_native("java/lang/StringBuffer", "length", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_string_buffers.find(args[0].as_ref());
            f.push_int(it != g_string_buffers.end()
                       ? utf8_char_count(it->second) : 0);
        });

    vm.register_native("java/lang/StringBuffer", "delete",
        "(II)Ljava/lang/StringBuffer;",
        [](VM&, Frame& f, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            int32_t from = args[1].as_int(), to = args[2].as_int();
            auto it = g_string_buffers.find(self);
            if (it != g_string_buffers.end()) {
                auto& s = it->second;
                size_t byte_from = utf8_char_to_byte(s, from);
                size_t byte_to   = utf8_char_to_byte(s, to);
                if (byte_from <= byte_to && byte_to <= s.size())
                    s.erase(byte_from, byte_to - byte_from);
            }
            f.push_ref(self);
        });

    vm.register_native("java/lang/StringBuffer", "setLength", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            int32_t len = args[1].as_int();
            if (len < 0) len = 0;
            auto& s = g_string_buffers[self];
            if ((int32_t)s.size() > len) s.resize(len);
            else while ((int32_t)s.size() < len) s += '\0';
        });

    vm.register_native("java/lang/StringBuffer", "setCharAt", "(IC)V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto it = g_string_buffers.find(args[0].as_ref());
            if (it == g_string_buffers.end()) return;
            int32_t idx = args[1].as_int();
            char ch = (char)args[2].as_int();
            auto& s = it->second;
            // Simple byte-level set for ASCII
            size_t off = utf8_char_to_byte(s, idx);
            if (off < s.size()) s[off] = ch;
        });

    vm.register_native("java/lang/StringBuffer", "charAt", "(I)C",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_string_buffers.find(args[0].as_ref());
            int32_t idx = args[1].as_int();
            if (it != g_string_buffers.end()) {
                size_t byte_off = utf8_char_to_byte(it->second, idx);
                if (byte_off < it->second.size())
                    f.push_int(utf8_decode_at(it->second, byte_off));
                else
                    f.push_int(0);
            } else {
                f.push_int(0);
            }
        });

    // ── java.lang.Short / Byte / Boolean ─────────────────────────────────────
    // Share g_integers storage — they all hold a small int value.
    vm.register_native("java/lang/Short", "<init>", "(S)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_integers[args[0].as_ref()] = (int16_t)args[1].as_int();
        });
    vm.register_native("java/lang/Short", "shortValue", "()S",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_integers.find(args[0].as_ref());
            f.push_int((int16_t)(it != g_integers.end() ? it->second : 0));
        });
    vm.register_native("java/lang/Short", "intValue", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_integers.find(args[0].as_ref());
            f.push_int(it != g_integers.end() ? it->second : 0);
        });
    vm.register_native("java/lang/Short", "toString", "()Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            auto it = g_integers.find(args[0].as_ref());
            f.push_ref(v.new_string(std::to_string(it != g_integers.end() ? it->second : 0)));
        });
    vm.register_native("java/lang/Byte", "<init>", "(B)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_integers[args[0].as_ref()] = (int8_t)args[1].as_int();
        });
    vm.register_native("java/lang/Byte", "byteValue", "()B",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_integers.find(args[0].as_ref());
            f.push_int((int8_t)(it != g_integers.end() ? it->second : 0));
        });
    vm.register_native("java/lang/Boolean", "<init>", "(Z)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_integers[args[0].as_ref()] = args[1].as_int() ? 1 : 0;
        });
    vm.register_native("java/lang/Boolean", "booleanValue", "()Z",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_integers.find(args[0].as_ref());
            f.push_int(it != g_integers.end() && it->second ? 1 : 0);
        });

    // ── java.lang.Integer ────────────────────────────────────────────────────

    vm.register_native("java/lang/Integer", "<init>", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_integers[args[0].as_ref()] = args[1].as_int();
        });

    vm.register_native("java/lang/Integer", "toString", "()Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            auto it = g_integers.find(args[0].as_ref());
            f.push_ref(v.new_string(std::to_string(it != g_integers.end() ? it->second : 0)));
        });

    vm.register_native("java/lang/Integer", "intValue", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_integers.find(args[0].as_ref());
            f.push_int(it != g_integers.end() ? it->second : 0);
        });

    vm.register_native("java/lang/Integer", "parseInt",
        "(Ljava/lang/String;)I",
        [](VM& v, Frame& f, std::span<Slot> args) {
            try { f.push_int(std::stoi(v.string_value(args[0].as_ref()))); }
            catch (...) { f.push_int(0); }
        });

    vm.register_native("java/lang/Integer", "parseInt",
        "(Ljava/lang/String;I)I",
        [](VM& v, Frame& f, std::span<Slot> args) {
            try {
                f.push_int(std::stoi(v.string_value(args[0].as_ref()),
                                     nullptr, args[1].as_int()));
            } catch (...) { f.push_int(0); }
        });

    vm.register_native("java/lang/Integer", "toString", "(I)Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            f.push_ref(v.new_string(std::to_string(args[0].as_int())));
        });

    vm.register_native("java/lang/Integer", "valueOf",
        "(I)Ljava/lang/Integer;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            ClassDef* klass = v.loader().find_or_stub("java/lang/Integer");
            ObjRef obj = v.heap().alloc_object(klass, 0);
            g_integers[obj] = args[0].as_int();
            f.push_ref(obj);
        });

    vm.register_native("java/lang/Integer", "valueOf",
        "(Ljava/lang/String;)Ljava/lang/Integer;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            ClassDef* klass = v.loader().find_or_stub("java/lang/Integer");
            ObjRef obj = v.heap().alloc_object(klass, 0);
            try { g_integers[obj] = std::stoi(v.string_value(args[0].as_ref())); }
            catch (...) { g_integers[obj] = 0; }
            f.push_ref(obj);
        });

    // ── java.util.Hashtable ───────────────────────────────────────────────────

    // Value-based equality for Hashtable keys (Integer, String, or reference)
    auto obj_equals = [](VM& v, ObjRef a, ObjRef b) -> bool {
        if (a == b) return true;
        if (a == NULL_REF || b == NULL_REF) return false;
        // Check Integer equality
        auto ia = g_integers.find(a), ib = g_integers.find(b);
        if (ia != g_integers.end() && ib != g_integers.end())
            return ia->second == ib->second;
        // Check String equality
        HeapObject* oa = v.heap().deref(a);
        HeapObject* ob = v.heap().deref(b);
        if (oa && ob && oa->klass && ob->klass &&
            oa->klass->name == "java/lang/String" &&
            ob->klass->name == "java/lang/String")
            return v.string_value(a) == v.string_value(b);
        return false;
    };

    vm.register_native("java/util/Hashtable", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_hashtables[args[0].as_ref()] = {};
        });

    vm.register_native("java/util/Hashtable", "put",
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;",
        [obj_equals](VM& v, Frame& f, std::span<Slot> args) {
            ObjRef self = args[0].as_ref(), key = args[1].as_ref(), val = args[2].as_ref();
            auto& map = g_hashtables[self];
            for (auto& [k, vi] : map) {
                if (obj_equals(v, k, key)) { ObjRef old = vi; vi = val; f.push_ref(old); return; }
            }
            map.push_back({key, val});
            f.push_ref(NULL_REF);
        });

    vm.register_native("java/util/Hashtable", "get",
        "(Ljava/lang/Object;)Ljava/lang/Object;",
        [obj_equals](VM& v, Frame& f, std::span<Slot> args) {
            ObjRef self = args[0].as_ref(), key = args[1].as_ref();
            auto& map = g_hashtables[self];
            for (auto& [k, vi] : map) {
                if (obj_equals(v, k, key)) { f.push_ref(vi); return; }
            }
            f.push_ref(NULL_REF);
        });

    vm.register_native("java/util/Hashtable", "containsKey",
        "(Ljava/lang/Object;)Z",
        [obj_equals](VM& v, Frame& f, std::span<Slot> args) {
            ObjRef self = args[0].as_ref(), key = args[1].as_ref();
            for (auto& [k, vi] : g_hashtables[self])
                if (obj_equals(v, k, key)) { f.push_int(1); return; }
            f.push_int(0);
        });

    vm.register_native("java/util/Hashtable", "size", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_hashtables.find(args[0].as_ref());
            f.push_int(it != g_hashtables.end()
                       ? static_cast<int32_t>(it->second.size()) : 0);
        });

    // ── java.util.Vector ─────────────────────────────────────────────────────

    vm.register_native("java/util/Vector", "<init>", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_vectors[args[0].as_ref()] = {};
        });
    vm.register_native("java/util/Vector", "<init>", "(II)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_vectors[args[0].as_ref()] = {};
        });
    vm.register_native("java/util/Vector", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_vectors[args[0].as_ref()] = {};
        });

    vm.register_native("java/util/Vector", "addElement",
        "(Ljava/lang/Object;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_vectors[args[0].as_ref()].push_back(args[1].as_ref());
        });

    vm.register_native("java/util/Vector", "elementAt",
        "(I)Ljava/lang/Object;",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto& v = g_vectors[args[0].as_ref()];
            int32_t idx = args[1].as_int();
            f.push_ref(idx >= 0 && idx < (int32_t)v.size() ? v[idx] : NULL_REF);
        });

    vm.register_native("java/util/Vector", "size", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_vectors.find(args[0].as_ref());
            f.push_int(it != g_vectors.end()
                       ? static_cast<int32_t>(it->second.size()) : 0);
        });

    vm.register_native("java/util/Vector", "removeAllElements", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_vectors[args[0].as_ref()].clear();
        });

    vm.register_native("java/util/Vector", "removeElementAt", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto& v = g_vectors[args[0].as_ref()];
            int32_t idx = args[1].as_int();
            if (idx >= 0 && idx < (int32_t)v.size())
                v.erase(v.begin() + idx);
        });

    vm.register_native("java/util/Vector", "insertElementAt",
        "(Ljava/lang/Object;I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto& v = g_vectors[args[0].as_ref()];
            ObjRef elem = args[1].as_ref();
            int32_t idx = args[2].as_int();
            if (idx >= 0 && idx <= (int32_t)v.size())
                v.insert(v.begin() + idx, elem);
            else
                v.push_back(elem);
        });

    vm.register_native("java/util/Vector", "isEmpty", "()Z",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_vectors.find(args[0].as_ref());
            f.push_int(it == g_vectors.end() || it->second.empty() ? 1 : 0);
        });

    vm.register_native("java/util/Vector", "contains",
        "(Ljava/lang/Object;)Z",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto& v = g_vectors[args[0].as_ref()];
            ObjRef elem = args[1].as_ref();
            for (auto r : v) if (r == elem) { f.push_int(1); return; }
            f.push_int(0);
        });

    vm.register_native("java/util/Vector", "removeElement",
        "(Ljava/lang/Object;)Z",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto& vec = g_vectors[args[0].as_ref()];
            ObjRef elem = args[1].as_ref();
            for (auto it = vec.begin(); it != vec.end(); ++it) {
                if (*it == elem) {
                    vec.erase(it);
                    f.push_int(1);
                    return;
                }
            }
            f.push_int(0);
        });

    vm.register_native("java/util/Vector", "indexOf",
        "(Ljava/lang/Object;)I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto& vec = g_vectors[args[0].as_ref()];
            ObjRef elem = args[1].as_ref();
            for (size_t i = 0; i < vec.size(); i++)
                if (vec[i] == elem) { f.push_int((int32_t)i); return; }
            f.push_int(-1);
        });

    vm.register_native("java/util/Vector", "lastElement",
        "()Ljava/lang/Object;",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto& vec = g_vectors[args[0].as_ref()];
            f.push_ref(vec.empty() ? NULL_REF : vec.back());
        });

    vm.register_native("java/util/Vector", "firstElement",
        "()Ljava/lang/Object;",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto& vec = g_vectors[args[0].as_ref()];
            f.push_ref(vec.empty() ? NULL_REF : vec.front());
        });

    vm.register_native("java/util/Vector", "ensureCapacity", "(I)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/util/Vector", "capacity", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto& vec = g_vectors[args[0].as_ref()];
            f.push_int(static_cast<int32_t>(vec.capacity()));
        });
    vm.register_native("java/util/Vector", "setSize", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto& vec = g_vectors[args[0].as_ref()];
            int32_t sz = args[1].as_int();
            if (sz < 0) sz = 0;
            vec.resize(sz, NULL_REF);
        });
    vm.register_native("java/util/Vector", "copyInto",
        "([Ljava/lang/Object;)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            auto& vec = g_vectors[args[0].as_ref()];
            ObjRef arr_ref = args[1].as_ref();
            HeapObject* arr = v.heap().deref(arr_ref);
            if (!arr) return;
            int32_t len = arr->array_length();
            Slot* slots = arr->array_slots();
            for (int32_t i = 0; i < std::min((int32_t)vec.size(), len); ++i)
                slots[i] = Slot::from_ref(vec[i]);
        });
    vm.register_native("java/util/Vector", "setElementAt",
        "(Ljava/lang/Object;I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto& vec = g_vectors[args[0].as_ref()];
            int idx = args[2].as_int();
            if (idx >= 0 && idx < (int)vec.size())
                vec[idx] = args[1].as_ref();
        });

    // Vector.elements() → Enumeration
    static std::unordered_map<ObjRef, std::pair<ObjRef, int32_t>> g_enumerations;  // enum ref → (vector ref, pos)

    vm.register_native("java/util/Vector", "elements",
        "()Ljava/util/Enumeration;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            ObjRef vec_ref = args[0].as_ref();
            ObjRef en = v.new_object(v.loader().find_or_stub("java/util/Enumeration"));
            g_enumerations[en] = {vec_ref, 0};
            f.push_ref(en);
        });

    vm.register_native("java/util/Enumeration", "hasMoreElements", "()Z",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_enumerations.find(args[0].as_ref());
            if (it == g_enumerations.end()) { f.push_int(0); return; }
            auto& vec = g_vectors[it->second.first];
            f.push_int(it->second.second < (int32_t)vec.size() ? 1 : 0);
        });

    vm.register_native("java/util/Enumeration", "nextElement",
        "()Ljava/lang/Object;",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_enumerations.find(args[0].as_ref());
            if (it == g_enumerations.end()) { f.push_ref(NULL_REF); return; }
            auto& vec = g_vectors[it->second.first];
            if (it->second.second < (int32_t)vec.size())
                f.push_ref(vec[it->second.second++]);
            else
                f.push_ref(NULL_REF);
        });

    // ── java.lang.Math ───────────────────────────────────────────────────────

    vm.register_native("java/lang/Math", "abs", "(I)I",
        [](VM&, Frame& f, std::span<Slot> args) {
            int32_t v = args[0].as_int();
            f.push_int(v < 0 ? -v : v);
        });
    vm.register_native("java/lang/Math", "min", "(II)I",
        [](VM&, Frame& f, std::span<Slot> args) {
            f.push_int(std::min(args[0].as_int(), args[1].as_int()));
        });
    vm.register_native("java/lang/Math", "max", "(II)I",
        [](VM&, Frame& f, std::span<Slot> args) {
            f.push_int(std::max(args[0].as_int(), args[1].as_int()));
        });

    // ── java.lang.Character ──────────────────────────────────────────────────
    // CLDC 1.1 limits these to ISO-8859-1. ASCII coverage suffices for the
    // games we run — stub was returning 0 which broke e.g. Character.digit-
    // based base-32 parsing in Spore.

    vm.register_native("java/lang/Character", "isLowerCase", "(C)Z",
        [](VM&, Frame& f, std::span<Slot> args) {
            int c = args[0].as_int() & 0xFFFF;
            f.push_int((c >= 'a' && c <= 'z') ? 1 : 0);
        });
    vm.register_native("java/lang/Character", "isUpperCase", "(C)Z",
        [](VM&, Frame& f, std::span<Slot> args) {
            int c = args[0].as_int() & 0xFFFF;
            f.push_int((c >= 'A' && c <= 'Z') ? 1 : 0);
        });
    vm.register_native("java/lang/Character", "isDigit", "(C)Z",
        [](VM&, Frame& f, std::span<Slot> args) {
            int c = args[0].as_int() & 0xFFFF;
            f.push_int((c >= '0' && c <= '9') ? 1 : 0);
        });
    vm.register_native("java/lang/Character", "digit", "(CI)I",
        [](VM&, Frame& f, std::span<Slot> args) {
            int c = args[0].as_int() & 0xFFFF;
            int radix = args[1].as_int();
            int v = -1;
            if (radix >= 2 && radix <= 36) {
                if (c >= '0' && c <= '9') v = c - '0';
                else if (c >= 'a' && c <= 'z') v = c - 'a' + 10;
                else if (c >= 'A' && c <= 'Z') v = c - 'A' + 10;
                if (v >= radix) v = -1;
            }
            f.push_int(v);
        });
    vm.register_native("java/lang/Character", "toLowerCase", "(C)C",
        [](VM&, Frame& f, std::span<Slot> args) {
            int c = args[0].as_int() & 0xFFFF;
            if (c >= 'A' && c <= 'Z') c += 32;
            f.push_int(c);
        });
    vm.register_native("java/lang/Character", "toUpperCase", "(C)C",
        [](VM&, Frame& f, std::span<Slot> args) {
            int c = args[0].as_int() & 0xFFFF;
            if (c >= 'a' && c <= 'z') c -= 32;
            f.push_int(c);
        });

    // ── java.lang.Runtime ──────────────────────────────────────────────────────

    vm.register_native("java/lang/Runtime", "getRuntime", "()Ljava/lang/Runtime;",
        [](VM& v, Frame& f, std::span<Slot>) {
            f.push_ref(v.new_object(v.loader().find_or_stub("java/lang/Runtime")));
        });
    vm.register_native("java/lang/Runtime", "freeMemory", "()J",
        [](VM&, Frame& f, std::span<Slot>) { f.push_long(1024 * 1024); });
    vm.register_native("java/lang/Runtime", "totalMemory", "()J",
        [](VM&, Frame& f, std::span<Slot>) { f.push_long(2 * 1024 * 1024); });
    vm.register_native("java/lang/Runtime", "gc", "()V",
        [](VM&, Frame&, std::span<Slot>) {});

    // ── java.lang.System ─────────────────────────────────────────────────────

    vm.register_native("java/lang/System", "currentTimeMillis", "()J",
        [](VM&, Frame& f, std::span<Slot>) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            f.push_long(int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000);
        });

    vm.register_native("java/lang/System", "arraycopy",
        "(Ljava/lang/Object;ILjava/lang/Object;II)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            ObjRef src = args[0].as_ref();
            int32_t sp = args[1].as_int();
            ObjRef dst = args[2].as_ref();
            int32_t dp = args[3].as_int();
            int32_t n  = args[4].as_int();
            HeapObject* s = v.heap().deref(src);
            HeapObject* d = v.heap().deref(dst);
            if (!s || !d || n <= 0) return;
            std::string kname = s->klass ? s->klass->name : "";
            if (kname == "[B") {
                std::memmove(d->array_bytes()  + dp, s->array_bytes()  + sp, n);
            } else if (kname == "[C" || kname == "[S") {
                std::memmove(d->array_shorts() + dp, s->array_shorts() + sp, n * 2);
            } else if (kname == "[J") {
                std::memmove(d->array_longs()  + dp, s->array_longs()  + sp, n * 8);
            } else {
                std::memmove(d->array_slots()  + dp, s->array_slots()  + sp, n * sizeof(Slot));
            }
        });

    vm.register_native("java/lang/System", "gc", "()V",
        [](VM&, Frame&, std::span<Slot>) {});

    // ── java.lang.Thread ─────────────────────────────────────────────────────

    vm.register_native("java/lang/Thread", "<init>", "(Ljava/lang/Runnable;)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/lang/Thread", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/lang/Thread", "start", "()V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/lang/Thread", "sleep", "(J)V",
        [](VM&, Frame&, std::span<Slot> args) {
            Slot2 s2; s2.lo = args[0].raw; s2.hi = args[1].raw;
            int64_t ms = s2.as_long();
            if (ms > 0 && ms <= 5000) {
                struct timespec ts { ms / 1000, (ms % 1000) * 1000000L };
                nanosleep(&ts, nullptr);
            }
        });
    vm.register_native("java/lang/Thread", "yield", "()V",
        [](VM& v, Frame&, std::span<Slot>) {
            // Pump SDL events + deliver input to the current displayable so
            // games whose main loop invalidates only on user input (wait for
            // keyPressed to flip a repaint-needed flag) aren't deadlocked.
            extern ObjRef g_current_displayable;
            Display& d = Display::instance();
            if (!d.is_open()) return;
            if (!d.flush()) throw QuitRequest{};
            ObjRef canvas = g_current_displayable;
            if (canvas == NULL_REF) return;
            auto presses  = d.take_key_presses();
            auto releases = d.take_key_releases();
            auto pointers = d.take_pointer_events();
            if (presses.empty() && releases.empty() && pointers.empty()) return;
            HeapObject* obj = v.heap().deref(canvas);
            if (!obj || !obj->klass) return;
            MethodDef* kp = obj->klass->resolve_virtual("keyPressed",  "(I)V");
            MethodDef* kr = obj->klass->resolve_virtual("keyReleased", "(I)V");
            MethodDef* pp = obj->klass->resolve_virtual("pointerPressed",  "(II)V");
            MethodDef* pr = obj->klass->resolve_virtual("pointerReleased", "(II)V");
            MethodDef* pd = obj->klass->resolve_virtual("pointerDragged",  "(II)V");
            for (int code : presses) if (kp) try {
                v.invoke(kp, obj->klass, {Slot::from_ref(canvas), Slot::from_int(code)});
            } catch (const QuitRequest&) { throw; } catch (...) {}
            for (int code : releases) if (kr) try {
                v.invoke(kr, obj->klass, {Slot::from_ref(canvas), Slot::from_int(code)});
            } catch (const QuitRequest&) { throw; } catch (...) {}
            for (auto& e : pointers) {
                MethodDef* m = nullptr;
                switch (e.kind) {
                    case Display::PointerKind::Pressed:  m = pp; break;
                    case Display::PointerKind::Released: m = pr; break;
                    case Display::PointerKind::Dragged:  m = pd; break;
                }
                if (!m) continue;
                try {
                    v.invoke(m, obj->klass, {Slot::from_ref(canvas),
                                              Slot::from_int(e.x), Slot::from_int(e.y)});
                } catch (const QuitRequest&) { throw; } catch (...) {}
            }
        });

    // ── javax.microedition.io.Connector ─────────────────────────────────────────
    // No real network. Return a fake HttpConnection that responds with HTTP 200
    // and "vserv:" headers — matches freej2me-plus's approach. Games using the
    // VServ ad SDK then think the ad fetch succeeded with no ad and proceed.
    auto make_fake_http = [](VM& v, std::span<Slot> args) {
        std::string url = v.string_value(args[0].as_ref());
        // For sms:// we return a MessageConnection so the cast in
        //   (MessageConnection) Connector.open("sms://…")
        // succeeds. Then newMessage/send below pretend the SMS went through,
        // which is what trial titles like KimCuong2 interpret as "registered".
        if (url.rfind("sms:", 0) == 0) {
            fprintf(stderr, "[net] Connector.open(%s) → fake MessageConnection\n",
                    url.c_str());
            ClassDef* mc = v.loader().find_or_stub(
                "javax/wireless/messaging/MessageConnection");
            return v.heap().alloc_object(mc, 0);
        }
        // tel:, cbs:, btspp:, … we don't simulate. Throw the standard
        // "transport unavailable" signal so games fall through.
        if (url.rfind("http:", 0) != 0 && url.rfind("https:", 0) != 0) {
            fprintf(stderr, "[net] Connector.open(%s) → ConnectionNotFoundException\n",
                    url.c_str());
            ClassDef* exk = v.loader().find_or_stub(
                "javax/microedition/io/ConnectionNotFoundException");
            ObjRef ex = v.heap().alloc_object(exk, 0);
            throw JvmException{ex,
                "ConnectionNotFoundException: " + url, {}};
        }
        fprintf(stderr, "[net] Connector.open(%s) → fake HTTP 200\n", url.c_str());
        ClassDef* httpKlass = v.loader().find_or_stub(
            "javax/microedition/io/HttpConnection");
        ObjRef ref = v.heap().alloc_object(httpKlass, 0);
        return ref;
    };
    vm.register_native("javax/microedition/io/Connector", "open",
        "(Ljava/lang/String;)Ljavax/microedition/io/Connection;",
        [make_fake_http](VM& v, Frame& f, std::span<Slot> args) {
            f.push_ref(make_fake_http(v, args));
        });
    vm.register_native("javax/microedition/io/Connector", "open",
        "(Ljava/lang/String;I)Ljavax/microedition/io/Connection;",
        [make_fake_http](VM& v, Frame& f, std::span<Slot> args) {
            f.push_ref(make_fake_http(v, args));
        });
    vm.register_native("javax/microedition/io/Connector", "open",
        "(Ljava/lang/String;IZ)Ljavax/microedition/io/Connection;",
        [make_fake_http](VM& v, Frame& f, std::span<Slot> args) {
            f.push_ref(make_fake_http(v, args));
        });

    // Fake HttpConnection methods.
    vm.register_native("javax/microedition/io/HttpConnection",
        "setRequestMethod", "(Ljava/lang/String;)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("javax/microedition/io/HttpConnection",
        "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("javax/microedition/io/HttpConnection",
        "getResponseCode", "()I",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(200); });
    vm.register_native("javax/microedition/io/HttpConnection",
        "getResponseMessage", "()Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot>) { f.push_ref(v.new_string("OK")); });
    vm.register_native("javax/microedition/io/HttpConnection",
        "getHeaderField", "(Ljava/lang/String;)Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            std::string name = v.string_value(args[1].as_ref());
            // Match freej2me-plus's vserv-bypass values.
            if (name == "location" || name == "Location")
                f.push_ref(v.new_string("vserv:"));
            else if (name == "X-VSERV-CONTEXT")
                f.push_ref(v.new_string("asd"));
            else
                f.push_ref(NULL_REF);
        });
    vm.register_native("javax/microedition/io/HttpConnection",
        "getLength", "()J",
        [](VM&, Frame& f, std::span<Slot>) { f.push_long(0); });

    // ── javax.wireless.messaging (WMA / JSR-120): fake SMS ─────────────
    // Trial-gated feature-phone games (KimCuong2, Vietnamese / Southeast
    // Asian titles) send a premium SMS to a short code to "register" and
    // unlock the full game. No real network — we just pretend every
    // newMessage→send round-trip succeeded. The game reads "SMS sent
    // successfully" and proceeds into gameplay. getAddress() returns the
    // sender's own "phone number" so round-trip reply games also work.
    auto new_fake_message = [](VM& v, Frame& f, std::span<Slot>) {
        ClassDef* tm = v.loader().find_or_stub(
            "javax/wireless/messaging/TextMessage");
        f.push_ref(v.heap().alloc_object(tm, 0));
    };
    vm.register_native("javax/wireless/messaging/MessageConnection",
        "newMessage", "(Ljava/lang/String;)Ljavax/wireless/messaging/Message;",
        new_fake_message);
    vm.register_native("javax/wireless/messaging/MessageConnection",
        "newMessage", "(Ljava/lang/String;Ljava/lang/String;)Ljavax/wireless/messaging/Message;",
        new_fake_message);
    vm.register_native("javax/wireless/messaging/MessageConnection",
        "send", "(Ljavax/wireless/messaging/Message;)V",
        [](VM&, Frame&, std::span<Slot>) {
            fprintf(stderr, "[net] MessageConnection.send → pretended success\n");
        });
    vm.register_native("javax/wireless/messaging/MessageConnection",
        "numberOfSegments", "(Ljavax/wireless/messaging/Message;)I",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(1); });
    vm.register_native("javax/wireless/messaging/MessageConnection",
        "close", "()V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("javax/wireless/messaging/MessageConnection",
        "setMessageListener", "(Ljavax/wireless/messaging/MessageListener;)V",
        [](VM&, Frame&, std::span<Slot>) {});
    // Message / TextMessage setters + getters (enough to round-trip).
    vm.register_native("javax/wireless/messaging/TextMessage",
        "setPayloadText", "(Ljava/lang/String;)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("javax/wireless/messaging/TextMessage",
        "getPayloadText", "()Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot>) { f.push_ref(v.new_string("")); });
    vm.register_native("javax/wireless/messaging/Message",
        "setAddress", "(Ljava/lang/String;)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("javax/wireless/messaging/Message",
        "getAddress", "()Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot>) { f.push_ref(v.new_string("sms://0")); });
    vm.register_native("javax/wireless/messaging/Message",
        "getTimestamp", "()Ljava/util/Date;",
        [](VM&, Frame& f, std::span<Slot>) { f.push_ref(NULL_REF); });
    vm.register_native("javax/microedition/io/HttpConnection",
        "openInputStream", "()Ljava/io/InputStream;",
        [](VM& v, Frame& f, std::span<Slot>) {
            // Return an empty InputStream
            ClassDef* isKlass = v.loader().find_or_stub("java/io/InputStream");
            ObjRef ref = v.heap().alloc_object(isKlass, 0);
            // Note: g_streams entry not added → reads return -1 (EOF)
            f.push_ref(ref);
        });
    vm.register_native("javax/microedition/io/HttpConnection",
        "openOutputStream", "()Ljava/io/OutputStream;",
        [](VM& v, Frame& f, std::span<Slot>) {
            ClassDef* osKlass = v.loader().find_or_stub("java/io/OutputStream");
            f.push_ref(v.heap().alloc_object(osKlass, 0));
        });
    vm.register_native("javax/microedition/io/HttpConnection",
        "close", "()V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/io/OutputStream",
        "write", "([B)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/io/OutputStream",
        "write", "([BII)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/io/OutputStream",
        "flush", "()V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/io/OutputStream",
        "close", "()V",
        [](VM&, Frame&, std::span<Slot>) {});

    // ── String.toString() ──────────────────────────────────────────────────────
    vm.register_native("java/lang/String", "toString", "()Ljava/lang/String;",
        [](VM&, Frame& f, std::span<Slot> args) {
            f.push_ref(args[0].as_ref());  // return this
        });

    // ── java.util.Date / Calendar ────────────────────────────────────────────
    vm.register_native("java/util/Date", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/util/Date", "<init>", "(J)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/util/Date", "getTime", "()J",
        [](VM&, Frame& f, std::span<Slot>) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            f.push_long(int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000);
        });

    vm.register_native("java/util/Calendar", "getInstance",
        "()Ljava/util/Calendar;",
        [](VM& v, Frame& f, std::span<Slot>) {
            f.push_ref(v.new_object(v.loader().find_or_stub("java/util/Calendar")));
        });
    vm.register_native("java/util/Calendar", "setTime", "(Ljava/util/Date;)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/util/Calendar", "getTime", "()Ljava/util/Date;",
        [](VM& v, Frame& f, std::span<Slot>) {
            ClassDef* dk = v.loader().find_or_stub("java/util/Date");
            f.push_ref(v.heap().alloc_object(dk, 0));
        });
    vm.register_native("java/util/Calendar", "setTimeInMillis", "(J)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("java/util/Calendar", "getTimeInMillis", "()J",
        [](VM&, Frame& f, std::span<Slot>) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            f.push_long(int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000);
        });
    vm.register_native("java/util/Calendar", "get", "(I)I",
        [](VM&, Frame& f, std::span<Slot> args) {
            // Calendar field IDs: YEAR=1, MONTH=2, DAY_OF_MONTH=5,
            // HOUR_OF_DAY=11, MINUTE=12, SECOND=13
            time_t now = time(nullptr);
            struct tm* t = localtime(&now);
            int field = args[1].as_int();
            int val = 0;
            switch (field) {
                case 1:  val = t->tm_year + 1900; break;  // YEAR
                case 2:  val = t->tm_mon;         break;  // MONTH (0-based)
                case 5:  val = t->tm_mday;        break;  // DAY_OF_MONTH
                case 7:  val = t->tm_wday + 1;    break;  // DAY_OF_WEEK
                case 11: val = t->tm_hour;        break;  // HOUR_OF_DAY
                case 12: val = t->tm_min;         break;  // MINUTE
                case 13: val = t->tm_sec;         break;  // SECOND
            }
            f.push_int(val);
        });

    // ── java.util.Random ──────────────────────────────────────────────────────

    vm.register_native("java/util/Random", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot>) { srand(static_cast<unsigned>(time(nullptr))); });
    vm.register_native("java/util/Random", "<init>", "(J)V",
        [](VM&, Frame&, std::span<Slot> args) {
            // args[0]=this, args[1]=lo word of seed, args[2]=hi word of seed
            Slot2 s; s.lo = args[1].raw; s.hi = args[2].raw;
            srand(static_cast<unsigned>(s.as_long() & 0xFFFFFFFF));
        });
    vm.register_native("java/util/Random", "nextInt", "(I)I",
        [](VM&, Frame& f, std::span<Slot> args) {
            int32_t b = args[1].as_int();
            f.push_int(b > 0 ? rand() % b : 0);
        });
    vm.register_native("java/util/Random", "nextInt", "()I",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(rand()); });

    // ── javax.microedition.midlet.MIDlet ─────────────────────────────────────

    vm.register_native("javax/microedition/midlet/MIDlet", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("javax/microedition/midlet/MIDlet",
        "getAppProperty", "(Ljava/lang/String;)Ljava/lang/String;",
        [&jar](VM& v, Frame& f, std::span<Slot> args) {
            std::string key = v.string_value(args[1].as_ref());
            // Read from JAR's MANIFEST.MF
            static std::unordered_map<std::string, std::string> manifest;
            static bool loaded = false;
            if (!loaded) {
                loaded = true;
                if (jar.has("META-INF/MANIFEST.MF")) {
                    const auto& data = jar.get("META-INF/MANIFEST.MF");
                    std::string mf(data.begin(), data.end());
                    // Simple parse: key: value per line
                    size_t pos = 0;
                    while (pos < mf.size()) {
                        size_t eol = mf.find_first_of("\r\n", pos);
                        if (eol == std::string::npos) eol = mf.size();
                        std::string line = mf.substr(pos, eol - pos);
                        size_t colon = line.find(':');
                        if (colon != std::string::npos) {
                            std::string k = line.substr(0, colon);
                            std::string val = line.substr(colon + 1);
                            // Trim leading space
                            while (!val.empty() && (val[0] == ' ' || val[0] == '\t'))
                                val.erase(0, 1);
                            manifest[k] = val;
                        }
                        pos = eol;
                        while (pos < mf.size() && (mf[pos] == '\r' || mf[pos] == '\n')) ++pos;
                    }
                }
            }
            auto it = manifest.find(key);
            if (it != manifest.end()) {
                f.push_ref(v.new_string(it->second));
            } else {
                f.push_ref(NULL_REF);
            }
        });
    vm.register_native("javax/microedition/midlet/MIDlet",
        "notifyDestroyed", "()V",
        [](VM&, Frame&, std::span<Slot>) {
            fprintf(stderr, "[midlet] notifyDestroyed() called\n");
            throw QuitRequest{};
        });

    // ── javax.microedition.lcdui.Display ──────────────────────────────────────

    vm.register_native("javax/microedition/lcdui/Display",
        "getDisplay",
        "(Ljavax/microedition/midlet/MIDlet;)Ljavax/microedition/lcdui/Display;",
        [](VM& v, Frame& f, std::span<Slot>) {
            f.push_ref(v.new_object(
                v.loader().find_or_stub("javax/microedition/lcdui/Display")));
        });
    vm.register_native("javax/microedition/lcdui/Display",
        "isColor", "()Z",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(1); });
    vm.register_native("javax/microedition/lcdui/Display",
        "vibrate", "(I)Z",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(0); });
    vm.register_native("javax/microedition/lcdui/Display",
        "flashBacklight", "(I)Z",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(0); });
    vm.register_native("javax/microedition/lcdui/Display",
        "numColors", "()I",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(65536); });

    vm.register_native("javax/microedition/lcdui/Display",
        "setCurrent", "(Ljavax/microedition/lcdui/Displayable;)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            ObjRef displayable = args[1].as_ref();
            if (displayable == NULL_REF) return;

            // Check if this is a List — if so, run an interactive menu loop
            auto lit = g_lists.find(displayable);
            if (lit != g_lists.end()) {
                ListData& ld = lit->second;
                Display& d = Display::instance();
                if (!d.is_open()) d.open(g_screen_w, g_screen_h);

                // Interactive list loop
                while (true) {
                    // Render
                    SDL_Surface* scr = d.screen();
                    SDL_FillRect(scr, nullptr, SDL_MapRGBA(scr->format, 255, 255, 255, 255));

                    int sw = scr->w, sh = scr->h;
                    uint32_t bgColor   = SDL_MapRGBA(scr->format, 255, 255, 255, 255);
                    uint32_t textColor = SDL_MapRGBA(scr->format, 0, 0, 0, 255);
                    uint32_t lineColor = SDL_MapRGBA(scr->format, 0x77, 0x77, 0x77, 255);
                    SDL_Color textSDL  = {0, 0, 0, 255};
                    SDL_Color whiteSDL = {255, 255, 255, 255};

                    TTF_Font* titleFont = get_ttf_font(11, false);
                    TTF_Font* itemFont  = get_ttf_font(11, false);
                    TTF_Font* cmdFont   = get_ttf_font(9, false);
                    int titleH  = titleFont ? TTF_FontHeight(titleFont) + 6 : 20;
                    int itemH   = itemFont  ? TTF_FontHeight(itemFont) + 4  : 16;
                    int cmdBarH = cmdFont   ? TTF_FontHeight(cmdFont) + 4   : 14;

                    // Title bar
                    if (titleFont && !ld.title.empty()) {
                        SDL_Surface* ts = TTF_RenderUTF8_Blended(titleFont, ld.title.c_str(), textSDL);
                        if (ts) {
                            SDL_Rect dr = {(sw - ts->w) / 2, 3, ts->w, ts->h};
                            SDL_BlitSurface(ts, nullptr, scr, &dr);
                            SDL_FreeSurface(ts);
                        }
                    }
                    // Title separator line (gray)
                    { SDL_Rect l = {0, titleH, sw, 1}; SDL_FillRect(scr, &l, lineColor); }

                    // Items (centered, black highlight for selected)
                    int itemsY = titleH + 2;
                    if (itemFont) {
                        for (int i = 0; i < (int)ld.items.size(); i++) {
                            bool sel = (i == ld.selected);
                            int y = itemsY + i * itemH;
                            if (sel) {
                                SDL_Rect hl = {0, y, sw, itemH};
                                SDL_FillRect(scr, &hl, textColor);
                            }
                            SDL_Surface* is = TTF_RenderUTF8_Blended(
                                itemFont, ld.items[i].c_str(), sel ? whiteSDL : textSDL);
                            if (is) {
                                SDL_Rect dr = {(sw - is->w) / 2, y + 2, is->w, is->h};
                                SDL_BlitSurface(is, nullptr, scr, &dr);
                                SDL_FreeSurface(is);
                            }
                        }
                    }

                    // Bottom command bar
                    int barY = sh - cmdBarH;
                    { SDL_Rect l = {0, barY, sw, 1}; SDL_FillRect(scr, &l, lineColor); }
                    if (cmdFont) {
                        // "X of N" centered
                        char counter[32];
                        snprintf(counter, sizeof(counter), "%d of %d",
                                 ld.selected + 1, (int)ld.items.size());
                        SDL_Surface* cs = TTF_RenderUTF8_Blended(cmdFont, counter, textSDL);
                        if (cs) {
                            SDL_Rect dr = {(sw - cs->w) / 2, barY + 2, cs->w, cs->h};
                            SDL_BlitSurface(cs, nullptr, scr, &dr);
                            SDL_FreeSurface(cs);
                        }
                        // "Exit" right-aligned
                        SDL_Surface* es = TTF_RenderUTF8_Blended(cmdFont, "Exit", textSDL);
                        if (es) {
                            SDL_Rect dr = {sw - es->w - 4, barY + 2, es->w, es->h};
                            SDL_BlitSurface(es, nullptr, scr, &dr);
                            SDL_FreeSurface(es);
                        }
                    }

                    if (!d.flush()) throw QuitRequest{};

                    // Handle input
                    auto presses = d.take_key_presses();
                    d.take_key_releases();  // drain
                    for (int key : presses) {
                        if (key == -1 && ld.selected > 0) ld.selected--;  // UP
                        if (key == -2 && ld.selected < (int)ld.items.size() - 1) ld.selected++;  // DOWN
                        if (key == -7) {  // SOFT2 = Exit
                            throw QuitRequest{};
                        }
                        if (key == -5 || key == -6) {  // FIRE or SOFT1 = select
                            // Fire commandAction on the listener
                            ObjRef listener = ld.command_listener;
                            if (listener == NULL_REF) {
                                auto clit = g_command_listeners.find(displayable);
                                if (clit != g_command_listeners.end())
                                    listener = clit->second;
                            }
                            if (listener != NULL_REF) {
                                HeapObject* lobj = v.heap().deref(listener);
                                if (lobj && lobj->klass) {
                                    MethodDef* ca = lobj->klass->resolve_virtual(
                                        "commandAction",
                                        "(Ljavax/microedition/lcdui/Command;Ljavax/microedition/lcdui/Displayable;)V");
                                    if (ca) {
                                        try {
                                            v.invoke(ca, lobj->klass, {
                                                Slot::from_ref(ld.command_listener),
                                                Slot::from_ref(ld.select_command),
                                                Slot::from_ref(displayable)
                                            });
                                        } catch (const QuitRequest&) { throw; }
                                        catch (...) {}
                                    }
                                }
                            }
                            return;  // exit list loop
                        }
                    }
                    SDL_Delay(30);
                }
                return;
            }

            // Not a List — remember as current canvas & call showNotify().
            extern ObjRef g_current_displayable;
            g_current_displayable = displayable;
            HeapObject* obj = v.heap().deref(displayable);
            if (!obj || !obj->klass) return;
            MethodDef* sn = obj->klass->resolve_virtual("showNotify", "()V");
            if (sn) {
                try {
                    v.invoke(sn, obj->klass, {Slot::from_ref(displayable)});
                } catch (const QuitRequest&) { throw; }
                catch (...) {}
            }
            // Real MIDP invokes paint() on the canvas immediately after
            // setCurrent so the user sees it without needing input. Older
            // titles (Puzzle Bobble 2003) do all their work in the MIDlet
            // constructor and leave startApp() empty — without this kick,
            // paint never fires, key delivery never starts, and the game
            // sits idle forever. Route through Canvas.repaint()V which our
            // natives resolve to do_repaint (lazy Display::open + paint +
            // flush).
            MethodDef* rp = obj->klass->resolve_virtual("repaint", "()V");
            if (rp) {
                try {
                    v.invoke(rp, obj->klass, {Slot::from_ref(displayable)});
                } catch (const QuitRequest&) { throw; }
                catch (...) {}
            }
        });

    // ── javax.microedition.lcdui.List ──────────────────────────────────────────

    vm.register_native("javax/microedition/lcdui/List",
        "<init>", "(Ljava/lang/String;I)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            ListData ld;
            ld.title = v.string_value(args[1].as_ref());
            ld.type = args[2].as_int();
            g_lists[self] = std::move(ld);
        });

    vm.register_native("javax/microedition/lcdui/List",
        "<init>", "(Ljava/lang/String;I[Ljava/lang/String;[Ljavax/microedition/lcdui/Image;)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            ObjRef self = args[0].as_ref();
            ListData ld;
            ld.title = v.string_value(args[1].as_ref());
            ld.type = args[2].as_int();
            ObjRef str_arr = args[3].as_ref();
            HeapObject* arr = v.heap().deref(str_arr);
            if (arr) {
                for (int32_t i = 0; i < arr->array_length(); i++)
                    ld.items.push_back(v.string_value(arr->array_slots()[i].as_ref()));
            }
            g_lists[self] = std::move(ld);
        });

    vm.register_native("javax/microedition/lcdui/List",
        "append", "(Ljava/lang/String;Ljavax/microedition/lcdui/Image;)I",
        [](VM& v, Frame& f, std::span<Slot> args) {
            auto it = g_lists.find(args[0].as_ref());
            if (it != g_lists.end()) {
                it->second.items.push_back(v.string_value(args[1].as_ref()));
                f.push_int((int32_t)it->second.items.size() - 1);
            } else {
                f.push_int(0);
            }
        });

    vm.register_native("javax/microedition/lcdui/List",
        "getSelectedIndex", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_lists.find(args[0].as_ref());
            f.push_int(it != g_lists.end() ? it->second.selected : 0);
        });

    vm.register_native("javax/microedition/lcdui/List",
        "setSelectedIndex", "(IZ)V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto it = g_lists.find(args[0].as_ref());
            if (it != g_lists.end()) it->second.selected = args[1].as_int();
        });

    vm.register_native("javax/microedition/lcdui/List",
        "size", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_lists.find(args[0].as_ref());
            f.push_int(it != g_lists.end() ? (int32_t)it->second.items.size() : 0);
        });

    vm.register_native("javax/microedition/lcdui/List",
        "getString", "(I)Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            auto it = g_lists.find(args[0].as_ref());
            int idx = args[1].as_int();
            if (it != g_lists.end() && idx >= 0 && idx < (int)it->second.items.size())
                f.push_ref(v.new_string(it->second.items[idx]));
            else
                f.push_ref(v.new_string(""));
        });

    vm.register_native("javax/microedition/lcdui/List",
        "delete", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto it = g_lists.find(args[0].as_ref());
            int idx = args[1].as_int();
            if (it != g_lists.end() && idx >= 0 && idx < (int)it->second.items.size())
                it->second.items.erase(it->second.items.begin() + idx);
        });

    vm.register_native("javax/microedition/lcdui/List",
        "deleteAll", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto it = g_lists.find(args[0].as_ref());
            if (it != g_lists.end()) it->second.items.clear();
        });

    // List inherits setCommandListener and addCommand from Displayable
    // Store the command listener for List objects
    vm.register_native("javax/microedition/lcdui/List",
        "setCommandListener", "(Ljavax/microedition/lcdui/CommandListener;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto it = g_lists.find(args[0].as_ref());
            if (it != g_lists.end()) it->second.command_listener = args[1].as_ref();
        });

    vm.register_native("javax/microedition/lcdui/List",
        "addCommand", "(Ljavax/microedition/lcdui/Command;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            // Store the first command as the select command
            auto it = g_lists.find(args[0].as_ref());
            if (it != g_lists.end() && it->second.select_command == NULL_REF)
                it->second.select_command = args[1].as_ref();
        });

    // List.SELECT_COMMAND — static field, used by IMPLICIT lists
    vm.register_native("javax/microedition/lcdui/List",
        "setSelectCommand", "(Ljavax/microedition/lcdui/Command;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto it = g_lists.find(args[0].as_ref());
            if (it != g_lists.end()) it->second.select_command = args[1].as_ref();
        });

    vm.register_native("javax/microedition/lcdui/Display",
        "callSerially", "(Ljava/lang/Runnable;)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            ObjRef runnable_ref = args[1].as_ref();
            if (runnable_ref == NULL_REF) return;
            HeapObject* obj = v.heap().deref(runnable_ref);
            if (!obj || !obj->klass) return;
            MethodDef* run = obj->klass->resolve_virtual("run", "()V");
            if (run) {
                // Defer like Thread.start() — run after current call chain completes
                v.enqueue_thread(runnable_ref, runnable_ref, run, obj->klass);
            }
        });

    // ── GameCanvas ────────────────────────────────────────────────────────────

    vm.register_native("javax/microedition/lcdui/game/GameCanvas",
        "<init>", "(Z)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("javax/microedition/lcdui/game/GameCanvas",
        "getKeyStates", "()I",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(0); });
    vm.register_native("javax/microedition/lcdui/game/GameCanvas",
        "flushGraphics", "()V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("javax/microedition/lcdui/game/GameCanvas",
        "getGraphics", "()Ljavax/microedition/lcdui/Graphics;",
        [](VM& v, Frame& f, std::span<Slot>) {
            f.push_ref(v.new_object(
                v.loader().find_or_stub("javax/microedition/lcdui/Graphics")));
        });

    // ── Canvas ────────────────────────────────────────────────────────────────

    vm.register_native("javax/microedition/lcdui/Canvas",
        "setFullScreenMode", "(Z)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("javax/microedition/lcdui/Canvas",
        "showNotify", "()V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("javax/microedition/lcdui/Canvas",
        "hideNotify", "()V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("javax/microedition/lcdui/Canvas",
        "sizeChanged", "(II)V",
        [](VM&, Frame&, std::span<Slot>) {});

    // Alert — stub dialogs as no-ops (games use these for info/error popups)
    vm.register_native("javax/microedition/lcdui/Alert",
        "<init>", "(Ljava/lang/String;)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("javax/microedition/lcdui/Alert",
        "<init>",
        "(Ljava/lang/String;Ljava/lang/String;Ljavax/microedition/lcdui/Image;Ljavax/microedition/lcdui/AlertType;)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("javax/microedition/lcdui/Alert",
        "setType", "(Ljavax/microedition/lcdui/AlertType;)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("javax/microedition/lcdui/Alert",
        "setTimeout", "(I)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("javax/microedition/lcdui/Alert",
        "setString", "(Ljava/lang/String;)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("javax/microedition/lcdui/Alert",
        "getDefaultTimeout", "()I",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(2000); });
    vm.register_native("javax/microedition/lcdui/Canvas",
        "getWidth", "()I",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(240); });
    vm.register_native("javax/microedition/lcdui/Canvas",
        "getHeight", "()I",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(320); });

    // getGameAction maps key codes to MIDP game actions.
    // MIDP game action constants: UP=1, LEFT=2, RIGHT=5, DOWN=6, FIRE=8, A=9, B=10, C=11, D=12
    // Key codes: UP=-1, DOWN=-2, LEFT=-3, RIGHT=-4, FIRE=-5; also numpad: 2/8/4/6/5
    vm.register_native("javax/microedition/lcdui/Canvas",
        "getGameAction", "(I)I",
        [](VM&, Frame& f, std::span<Slot> args) {
            int32_t key = args[1].as_int();
            int32_t action = 0;
            switch (key) {
                case -1: case '2': action = 1; break;  // UP
                case -3: case '4': action = 2; break;  // LEFT
                case -4: case '6': action = 5; break;  // RIGHT
                case -2: case '8': action = 6; break;  // DOWN
                case -5: case '5': action = 8; break;  // FIRE
            }
            f.push_int(action);
        });

    // ── javax.microedition.rms.RecordStore ───────────────────────────────────
    // Simple in-memory record store: g_record_stores[name] = list of byte arrays.
    // Records are 1-based: id 1 = index 0.
    // g_recordstore_names maps ObjRef -> store name.

    // Helper to look up store name from a RecordStore ObjRef
    auto rs_name = [](ObjRef ref) -> const std::string& {
        static std::string empty;
        auto it = g_recordstore_names.find(ref);
        return it != g_recordstore_names.end() ? it->second : empty;
    };

    vm.register_native("javax/microedition/rms/RecordStore",
        "openRecordStore",
        "(Ljava/lang/String;Z)Ljavax/microedition/rms/RecordStore;",
        [rs_name](VM& v, Frame& f, std::span<Slot> args) {
            std::string name = v.string_value(args[0].as_ref());
            if (g_record_stores.find(name) == g_record_stores.end()) {
                g_record_stores[name]; // create entry
                rms_load(name);        // load from disk if available
            }
            ObjRef rs = v.heap().alloc_object(
                v.loader().find_or_stub("javax/microedition/rms/RecordStore"), 0);
            g_recordstore_names[rs] = name;
            f.push_ref(rs);
        });

    vm.register_native("javax/microedition/rms/RecordStore",
        "getNumRecords", "()I",
        [rs_name](VM&, Frame& f, std::span<Slot> args) {
            const std::string& name = rs_name(args[0].as_ref());
            auto it = g_record_stores.find(name);
            f.push_int(it != g_record_stores.end() ? (int32_t)it->second.size() : 0);
        });

    vm.register_native("javax/microedition/rms/RecordStore",
        "addRecord", "([BII)I",
        [rs_name](VM& v, Frame& f, std::span<Slot> args) {
            const std::string& name = rs_name(args[0].as_ref());
            ObjRef arr = args[1].as_ref();
            int32_t offset = args[2].as_int();
            int32_t length = args[3].as_int();
            auto& recs = g_record_stores[name];
            HeapObject* arrObj = v.heap().deref(arr);
            std::vector<uint8_t> bytes;
            if (arrObj && length > 0) {
                uint8_t* data = arrObj->array_bytes();
                bytes.assign(data + offset, data + offset + length);
            }
            recs.push_back(std::move(bytes));
            f.push_int((int32_t)recs.size());
            rms_save(name);
        });

    vm.register_native("javax/microedition/rms/RecordStore",
        "getRecord", "(I)[B",
        [rs_name](VM& v, Frame& f, std::span<Slot> args) {
            const std::string& name = rs_name(args[0].as_ref());
            int32_t id = args[1].as_int();
            auto it = g_record_stores.find(name);
            if (it == g_record_stores.end() || id < 1 || id > (int32_t)it->second.size()) {
                f.push_ref(NULL_REF); return;
            }
            const auto& bytes = it->second[id - 1];
            ObjRef arr = v.heap().alloc_prim_array(ArrayType::Byte,
                (int32_t)bytes.size(), v.loader().find_or_stub("[B"));
            HeapObject* arrObj = v.heap().deref(arr);
            if (arrObj && !bytes.empty())
                std::memcpy(arrObj->array_bytes(), bytes.data(), bytes.size());
            f.push_ref(arr);
        });

    vm.register_native("javax/microedition/rms/RecordStore",
        "setRecord", "(I[BII)V",
        [rs_name](VM& v, Frame&, std::span<Slot> args) {
            const std::string& name = rs_name(args[0].as_ref());
            int32_t id = args[1].as_int();
            ObjRef arr = args[2].as_ref();
            int32_t offset = args[3].as_int();
            int32_t length = args[4].as_int();
            auto& recs = g_record_stores[name];
            if (id >= 1 && id <= (int32_t)recs.size()) {
                HeapObject* arrObj = v.heap().deref(arr);
                if (arrObj && length > 0) {
                    uint8_t* data = arrObj->array_bytes();
                    recs[id - 1].assign(data + offset, data + offset + length);
                }
            }
            rms_save(name);
        });

    vm.register_native("javax/microedition/rms/RecordStore",
        "deleteRecord", "(I)V",
        [rs_name](VM&, Frame&, std::span<Slot> args) {
            const std::string& name = rs_name(args[0].as_ref());
            int32_t id = args[1].as_int();
            auto& recs = g_record_stores[name];
            if (id >= 1 && id <= (int32_t)recs.size())
                recs[id - 1].clear();
            rms_save(name);
        });

    vm.register_native("javax/microedition/rms/RecordStore",
        "closeRecordStore", "()V",
        [](VM&, Frame&, std::span<Slot>) {});

    vm.register_native("javax/microedition/rms/RecordStore",
        "deleteRecordStore", "(Ljava/lang/String;)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            std::string name = v.string_value(args[0].as_ref());
            g_record_stores.erase(name);
            auto path = rms_path(name);
            if (fs::exists(path)) fs::remove(path);
        });

    vm.register_native("javax/microedition/rms/RecordStore",
        "getNextRecordID", "()I",
        [rs_name](VM&, Frame& f, std::span<Slot> args) {
            const std::string& name = rs_name(args[0].as_ref());
            auto it = g_record_stores.find(name);
            f.push_int(it != g_record_stores.end() ? (int32_t)it->second.size() + 1 : 1);
        });

    vm.register_native("javax/microedition/rms/RecordStore",
        "listRecordStores", "()[Ljava/lang/String;",
        [](VM& v, Frame& f, std::span<Slot>) {
            // List all record stores on disk + in memory
            std::vector<std::string> names;
            for (auto& [name, _] : g_record_stores)
                names.push_back(name);
            // Also scan the RMS directory for persisted stores
            if (!g_rms_dir.empty() && fs::exists(g_rms_dir)) {
                for (auto& entry : fs::directory_iterator(g_rms_dir)) {
                    if (entry.path().extension() == ".rms") {
                        std::string name = entry.path().stem().string();
                        if (g_record_stores.find(name) == g_record_stores.end())
                            names.push_back(name);
                    }
                }
            }
            if (names.empty()) { f.push_ref(NULL_REF); return; }
            ObjRef arr = v.heap().alloc_prim_array(ArrayType::Ref,
                (int32_t)names.size(),
                v.loader().find_or_stub("[Ljava/lang/String;"));
            HeapObject* arrObj = v.heap().deref(arr);
            for (int32_t i = 0; i < (int32_t)names.size(); i++)
                arrObj->array_slots()[i] = Slot::from_ref(v.new_string(names[i]));
            f.push_ref(arr);
        });

    vm.register_native("javax/microedition/rms/RecordStore",
        "getRecordSize", "(I)I",
        [rs_name](VM&, Frame& f, std::span<Slot> args) {
            const std::string& name = rs_name(args[0].as_ref());
            int32_t id = args[1].as_int();
            auto it = g_record_stores.find(name);
            if (it == g_record_stores.end() || id < 1 || id > (int32_t)it->second.size()) {
                f.push_int(0); return;
            }
            f.push_int((int32_t)it->second[id - 1].size());
        });

    // ── RecordEnumeration ──────────────────────────────────────────────────────
    // Simple iterator: tracks store name + current position.
    struct RecordEnum { std::string store; int32_t pos; };
    static std::unordered_map<ObjRef, RecordEnum> g_record_enums;

    vm.register_native("javax/microedition/rms/RecordStore",
        "enumerateRecords",
        "(Ljavax/microedition/rms/RecordFilter;Ljavax/microedition/rms/RecordComparator;Z)Ljavax/microedition/rms/RecordEnumeration;",
        [rs_name](VM& v, Frame& f, std::span<Slot> args) {
            const std::string& name = rs_name(args[0].as_ref());
            // args[1]=filter, args[2]=comparator, args[3]=keepUpdated — all ignored
            ObjRef enumRef = v.new_object(
                v.loader().find_or_stub("javax/microedition/rms/RecordEnumeration"));
            g_record_enums[enumRef] = {name, 0};
            f.push_ref(enumRef);
        });

    vm.register_native("javax/microedition/rms/RecordEnumeration",
        "hasNextElement", "()Z",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_record_enums.find(args[0].as_ref());
            if (it == g_record_enums.end()) { f.push_int(0); return; }
            auto& re = it->second;
            auto sit = g_record_stores.find(re.store);
            // Skip deleted (empty) records
            if (sit != g_record_stores.end()) {
                while (re.pos < (int32_t)sit->second.size() && sit->second[re.pos].empty())
                    re.pos++;
                f.push_int(re.pos < (int32_t)sit->second.size() ? 1 : 0);
            } else {
                f.push_int(0);
            }
        });

    vm.register_native("javax/microedition/rms/RecordEnumeration",
        "nextRecordId", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_record_enums.find(args[0].as_ref());
            if (it == g_record_enums.end()) { f.push_int(-1); return; }
            auto& re = it->second;
            auto sit = g_record_stores.find(re.store);
            if (sit != g_record_stores.end()) {
                while (re.pos < (int32_t)sit->second.size() && sit->second[re.pos].empty())
                    re.pos++;
                if (re.pos < (int32_t)sit->second.size()) {
                    f.push_int(re.pos + 1);  // 1-based ID
                    re.pos++;
                    return;
                }
            }
            f.push_int(-1);
        });

    vm.register_native("javax/microedition/rms/RecordEnumeration",
        "numRecords", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            auto it = g_record_enums.find(args[0].as_ref());
            if (it == g_record_enums.end()) { f.push_int(0); return; }
            auto sit = g_record_stores.find(it->second.store);
            if (sit == g_record_stores.end()) { f.push_int(0); return; }
            int32_t count = 0;
            for (auto& r : sit->second) if (!r.empty()) count++;
            f.push_int(count);
        });

    vm.register_native("javax/microedition/rms/RecordEnumeration",
        "destroy", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_record_enums.erase(args[0].as_ref());
        });

    vm.register_native("javax/microedition/rms/RecordEnumeration",
        "reset", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto it = g_record_enums.find(args[0].as_ref());
            if (it != g_record_enums.end()) it->second.pos = 0;
        });

    // ── PrintStream ───────────────────────────────────────────────────────────

    vm.register_native("java/io/PrintStream", "println",
        "(Ljava/lang/String;)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            printf("[j2me] %s\n", v.string_value(args[1].as_ref()).c_str());
        });
    vm.register_native("java/io/PrintStream", "println", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            printf("[j2me] %d\n", args[1].as_int()); });
    vm.register_native("java/io/PrintStream", "println", "()V",
        [](VM&, Frame&, std::span<Slot>) { printf("[j2me]\n"); });
    vm.register_native("java/io/PrintStream", "print",
        "(Ljava/lang/String;)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            printf("[j2me] %s", v.string_value(args[1].as_ref()).c_str());
        });

    // ── javax.microedition.media (MMAPI) ─────────────────────────────────────
    // Each Player is backed by either a Mix_Music* (MIDI) or Mix_Chunk* (WAV).

    struct PlayerData {
        Mix_Music* music = nullptr;   // for MIDI
        Mix_Chunk* chunk = nullptr;   // for WAV/PCM
        int loop_count = 1;           // -1 = indefinite
        int channel = -1;             // assigned mixer channel for chunks
        SDL_RWops* rw = nullptr;      // keep alive for SDL_mixer
        std::vector<uint8_t> buf;     // owns the byte data
    };
    static std::unordered_map<ObjRef, PlayerData> g_players;
    static bool g_mixer_inited = false;

    auto ensure_mixer = []() {
        if (g_mixer_inited) return;
        if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0)
            fprintf(stderr, "[audio] Mix_OpenAudio failed: %s\n", Mix_GetError());
        Mix_AllocateChannels(16);
        g_mixer_inited = true;
    };

    // Manager.createPlayer(InputStream, String contentType) → Player
    vm.register_native("javax/microedition/media/Manager",
        "createPlayer",
        "(Ljava/io/InputStream;Ljava/lang/String;)Ljavax/microedition/media/Player;",
        [ensure_mixer](VM& v, Frame& f, std::span<Slot> args) {
            ObjRef stream_ref = args[0].as_ref();
            std::string content_type = v.string_value(args[1].as_ref());
            // Read all bytes from the stream
            auto it = g_streams.find(stream_ref);
            if (it == g_streams.end()) {
                // Return a valid but empty player so the game doesn't think sound init failed
                // Return a valid but empty player so the game doesn't think sound init failed
                ObjRef dummy = v.new_object(
                    v.loader().find_or_stub("javax/microedition/media/Player"));
                g_players[dummy] = PlayerData{};
                f.push_ref(dummy);
                return;
            }
            StreamEntry& se = it->second;
            std::vector<uint8_t> data(se.data.begin() + se.pos, se.data.end());
            se.pos = (int32_t)se.data.size();

            ensure_mixer();

            ObjRef player_ref = v.new_object(
                v.loader().find_or_stub("javax/microedition/media/Player"));
            PlayerData pd;
            pd.buf = std::move(data);
            pd.rw = SDL_RWFromConstMem(pd.buf.data(), (int)pd.buf.size());

            bool is_midi = content_type.find("midi") != std::string::npos ||
                           content_type.find("MIDI") != std::string::npos;
            // Also detect by file signature
            if (!is_midi && pd.buf.size() >= 4 &&
                pd.buf[0] == 'M' && pd.buf[1] == 'T' &&
                pd.buf[2] == 'h' && pd.buf[3] == 'd')
                is_midi = true;

            if (is_midi) {
                pd.music = Mix_LoadMUS_RW(pd.rw, 0);
                if (!pd.music)
                    fprintf(stderr, "[audio] Mix_LoadMUS_RW failed: %s\n", Mix_GetError());
            } else {
                pd.chunk = Mix_LoadWAV_RW(pd.rw, 0);
                if (!pd.chunk)
                    fprintf(stderr, "[audio] Mix_LoadWAV_RW failed: %s\n", Mix_GetError());
            }

            g_players[player_ref] = std::move(pd);
            f.push_ref(player_ref);
        });

    // Manager.createPlayer(String locator) → Player
    vm.register_native("javax/microedition/media/Manager",
        "createPlayer",
        "(Ljava/lang/String;)Ljavax/microedition/media/Player;",
        [](VM& v, Frame& f, std::span<Slot>) {
            // Locator-based creation (e.g. "capture://audio") — stub
            f.push_ref(v.new_object(
                v.loader().find_or_stub("javax/microedition/media/Player")));
        });

    static std::unordered_map<ObjRef, ObjRef> g_player_listeners;

    vm.register_native("javax/microedition/media/Player",
        "addPlayerListener",
        "(Ljavax/microedition/media/PlayerListener;)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            ObjRef player_ref = args[0].as_ref();
            ObjRef listener_ref = args[1].as_ref();
            g_player_listeners[player_ref] = listener_ref;
            // Do NOT fire "deviceAvailable" here. Per JSR-135 this event is only
            // sent when the audio device recovers from being claimed by another
            // MIDlet — never at registration. Firing it here causes Age of
            // Empires II's b.a/playerUpdate handler to set g.ck=true, which
            // triggers a hardcoded Chinese "resume game?" pause dialog.
        });

    vm.register_native("javax/microedition/media/Player",
        "removePlayerListener",
        "(Ljavax/microedition/media/PlayerListener;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            g_player_listeners.erase(args[0].as_ref());
        });

    vm.register_native("javax/microedition/media/Player",
        "realize", "()V",
        [](VM&, Frame&, std::span<Slot>) {
            // No-op: deviceAvailable is not part of realize() per JSR-135.
        });

    vm.register_native("javax/microedition/media/Player",
        "prefetch", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
        });

    vm.register_native("javax/microedition/media/Player",
        "start", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto it = g_players.find(args[0].as_ref());
            if (it == g_players.end()) return;
            PlayerData& pd = it->second;
            int loops = (pd.loop_count == -1) ? -1 : pd.loop_count - 1;
            if (pd.music) {
                Mix_PlayMusic(pd.music, loops);
            } else if (pd.chunk) {
                pd.channel = Mix_PlayChannel(-1, pd.chunk, loops);
            }
        });

    vm.register_native("javax/microedition/media/Player",
        "stop", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto it = g_players.find(args[0].as_ref());
            if (it == g_players.end()) return;
            PlayerData& pd = it->second;
            if (pd.music) Mix_HaltMusic();
            else if (pd.channel >= 0) Mix_HaltChannel(pd.channel);
        });

    vm.register_native("javax/microedition/media/Player",
        "close", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto it = g_players.find(args[0].as_ref());
            if (it == g_players.end()) return;
            PlayerData& pd = it->second;
            if (pd.music)  { Mix_HaltMusic(); Mix_FreeMusic(pd.music); }
            if (pd.chunk)  { if (pd.channel >= 0) Mix_HaltChannel(pd.channel); Mix_FreeChunk(pd.chunk); }
            if (pd.rw)     SDL_RWclose(pd.rw);
            g_players.erase(it);
        });

    vm.register_native("javax/microedition/media/Player",
        "deallocate", "()V",
        [](VM&, Frame&, std::span<Slot>) {});

    vm.register_native("javax/microedition/media/Player",
        "setLoopCount", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto it = g_players.find(args[0].as_ref());
            if (it != g_players.end())
                it->second.loop_count = args[1].as_int();
        });

    vm.register_native("javax/microedition/media/Player",
        "getState", "()I",
        [](VM&, Frame& f, std::span<Slot>) {
            f.push_int(300); // PREFETCHED
        });

    // getControl("VolumeControl") → returns a VolumeControl object
    vm.register_native("javax/microedition/media/Player",
        "getControl", "(Ljava/lang/String;)Ljavax/microedition/media/Control;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            // Return a VolumeControl object linked to this player
            ObjRef vc = v.new_object(
                v.loader().find_or_stub("javax/microedition/media/control/VolumeControl"));
            // Stash the player ref so setLevel can find it
            static std::unordered_map<ObjRef, ObjRef> g_vc_player;
            g_vc_player[vc] = args[0].as_ref();
            f.push_ref(vc);
        });

    vm.register_native("javax/microedition/media/control/VolumeControl",
        "setLevel", "(I)I",
        [](VM&, Frame& f, std::span<Slot> args) {
            int level = args[1].as_int(); // 0-100
            // Set global volume (0-128 for SDL_mixer)
            int vol = level * MIX_MAX_VOLUME / 100;
            Mix_VolumeMusic(vol);
            Mix_Volume(-1, vol); // all channels
            f.push_int(level);
        });

    vm.register_native("javax/microedition/media/control/VolumeControl",
        "getLevel", "()I",
        [](VM&, Frame& f, std::span<Slot>) {
            f.push_int(Mix_VolumeMusic(-1) * 100 / MIX_MAX_VOLUME);
        });
}
