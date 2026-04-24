// Native-method bridge from javax/microedition/m3g/* to the vendored M3G
// reference (third_party/m3g). Each Java object backed by an M3G handle
// stored in g_m3g_handles, keyed by its ObjRef.
//
// What's wired so far (in scan order of demand):
//   Graphics3D      — singleton, bindTarget, releaseTarget, render
//   Camera          — <init>, setPerspective, setParallel, setGeneric
//   Background      — <init>, setColor
//   Transform       — <init>, setIdentity, postTranslate, postRotate,
//                     postScale, transpose
//   Appearance      — <init>, setTexture, setCompositingMode, setMaterial
//   CompositingMode — <init>, setBlending, setAlphaThreshold
//   Texture2D       — <init>(Image2D), setBlending
//   Image2D         — <init>(int,Object) — loads from byte[] or RGB array
//   World           — <init>, setActiveCamera, setBackground
//   Light           — <init>, setMode, setColor, setIntensity
//
// Missing / not yet wired (smaller demand, defer):
//   Mesh / SkinnedMesh / MorphingMesh, Sprite3D, KeyframeSequence,
//   AnimationController, Loader.load(String), Material, Fog,
//   Group.addChild, Node.transform setters, RayIntersection.

#include "m3g_core.h"
#include "../vm/vm.hpp"
#include "natives.hpp"

#include <cstdio>
#include <cstdint>
#include <unordered_map>

// Provided by m3g_backend.cpp
extern M3GInterface j2me_m3g_interface();
extern bool         j2me_m3g_make_current();

namespace {

// One handle map covers every M3G class — we pun the M3G handles to a
// uintptr_t for storage.
std::unordered_map<ObjRef, uintptr_t> g_m3g_handles;

template <typename Handle>
Handle handle_for(ObjRef ref) {
    auto it = g_m3g_handles.find(ref);
    return it == g_m3g_handles.end() ? (Handle)0 : (Handle)it->second;
}

void store_handle(ObjRef ref, uintptr_t h) {
    g_m3g_handles[ref] = h;
}

// Process-wide singleton render context (one Graphics3D in MIDP)
M3GRenderContext g_render_context = (M3GRenderContext)0;

M3GRenderContext ensure_render_context() {
    if (g_render_context) return g_render_context;
    M3GInterface itf = j2me_m3g_interface();
    if (!itf) return (M3GRenderContext)0;
    g_render_context = m3gCreateContext(itf);
    if (!g_render_context)
        std::fprintf(stderr, "[m3g] m3gCreateContext failed\n");
    return g_render_context;
}

// Helper: create an M3G object via factory func, stash by ObjRef.
template <typename Factory>
void create_into(ObjRef self, Factory factory) {
    M3GInterface itf = j2me_m3g_interface();
    if (!itf) return;
    auto h = factory(itf);
    if (h) store_handle(self, (uintptr_t)h);
}

} // namespace

// Set J2ME_TRACE_M3G=1 to log every M3G native call — used when diagnosing
// what a 3D game is (or isn't) actually asking for at its render path.
#define M3G_TRACE(what) do { \
    static bool s_enabled_##__LINE__ = (std::getenv("J2ME_TRACE_M3G") != nullptr); \
    if (s_enabled_##__LINE__) std::fprintf(stderr, "[m3g] " what "\n"); \
} while(0)

// Map an M3G class enum (from m3gGetClass) to its JSR-184 Java class name.
// Loader.load returns a heterogeneous Object3D[]; each element's dynamic
// Java type has to match the underlying M3G object so game code can
// downcast it (e.g. `(Mesh)loaded[3]`).
static const char* m3g_java_class_for(M3GClass c) {
    switch (c) {
        case M3G_CLASS_ANIMATION_CONTROLLER: return "javax/microedition/m3g/AnimationController";
        case M3G_CLASS_ANIMATION_TRACK:      return "javax/microedition/m3g/AnimationTrack";
        case M3G_CLASS_APPEARANCE:           return "javax/microedition/m3g/Appearance";
        case M3G_CLASS_BACKGROUND:           return "javax/microedition/m3g/Background";
        case M3G_CLASS_CAMERA:               return "javax/microedition/m3g/Camera";
        case M3G_CLASS_COMPOSITING_MODE:     return "javax/microedition/m3g/CompositingMode";
        case M3G_CLASS_FOG:                  return "javax/microedition/m3g/Fog";
        case M3G_CLASS_GROUP:                return "javax/microedition/m3g/Group";
        case M3G_CLASS_IMAGE:                return "javax/microedition/m3g/Image2D";
        case M3G_CLASS_INDEX_BUFFER:         return "javax/microedition/m3g/TriangleStripArray";
        case M3G_CLASS_KEYFRAME_SEQUENCE:    return "javax/microedition/m3g/KeyframeSequence";
        case M3G_CLASS_LIGHT:                return "javax/microedition/m3g/Light";
        case M3G_CLASS_MATERIAL:             return "javax/microedition/m3g/Material";
        case M3G_CLASS_MESH:                 return "javax/microedition/m3g/Mesh";
        case M3G_CLASS_MORPHING_MESH:        return "javax/microedition/m3g/MorphingMesh";
        case M3G_CLASS_POLYGON_MODE:         return "javax/microedition/m3g/PolygonMode";
        case M3G_CLASS_SKINNED_MESH:         return "javax/microedition/m3g/SkinnedMesh";
        case M3G_CLASS_SPRITE:               return "javax/microedition/m3g/Sprite3D";
        case M3G_CLASS_TEXTURE:              return "javax/microedition/m3g/Texture2D";
        case M3G_CLASS_VERTEX_ARRAY:         return "javax/microedition/m3g/VertexArray";
        case M3G_CLASS_VERTEX_BUFFER:        return "javax/microedition/m3g/VertexBuffer";
        case M3G_CLASS_WORLD:                return "javax/microedition/m3g/World";
        default:                             return "javax/microedition/m3g/Object3D";
    }
}

// Feed `data` (a .m3g byte stream) through a fresh M3GLoader, import the
// loaded objects into the main interface, and build a Java Object3D[]
// whose element types mirror each underlying M3G class. Returns NULL_REF
// on any failure (game treats null-element array as empty).
static ObjRef m3g_loader_load(VM& v, const uint8_t* data, size_t len) {
    M3GInterface itf = j2me_m3g_interface();
    ClassDef* arr_klass = v.loader().find_or_stub("[Ljavax/microedition/m3g/Object3D;");
    if (!itf) return v.heap().alloc_ref_array(0, arr_klass);

    M3GLoader loader = m3gCreateLoader(itf);
    if (!loader) return v.heap().alloc_ref_array(0, arr_klass);

    // Feed bytes in one chunk. m3gDecodeData returns bytes consumed; loop
    // until all consumed or zero-progress (malformed / truncated stream).
    M3Gsizei offset = 0;
    while (offset < (M3Gsizei)len) {
        M3Gsizei n = m3gDecodeData(loader, (M3Gsizei)len - offset, data + offset);
        if (n <= 0) break;
        offset += n;
    }

    M3Gint count = m3gGetLoadedObjects(loader, nullptr);
    if (count <= 0) return v.heap().alloc_ref_array(0, arr_klass);

    std::vector<M3Gulong> refs((size_t)count);
    m3gGetLoadedObjects(loader, refs.data());
    m3gImportObjects(loader, count, refs.data());

    ObjRef arr = v.heap().alloc_ref_array(count, arr_klass);
    HeapObject* arr_obj = v.heap().deref(arr);
    if (!arr_obj) return v.heap().alloc_ref_array(0, arr_klass);
    for (M3Gint i = 0; i < count; ++i) {
        auto obj = (M3GObject)(uintptr_t)refs[i];
        M3GClass cls = m3gGetClass(obj);
        ClassDef* jcls = v.loader().find_or_stub(m3g_java_class_for(cls));
        ObjRef jref = v.heap().alloc_object(jcls, 0);
        store_handle(jref, (uintptr_t)obj);
        arr_obj->array_slots()[i] = Slot::from_ref(jref);
    }
    return arr;
}

void register_m3g_natives(VM& vm, const JarFile& jar) {

    // ── Graphics3D ────────────────────────────────────────────────────────────
    // Singleton — getInstance() always returns the same Java object backed
    // by the process-wide M3GRenderContext.
    vm.register_native("javax/microedition/m3g/Graphics3D",
        "getInstance", "()Ljavax/microedition/m3g/Graphics3D;",
        [](VM& v, Frame& f, std::span<Slot>) {
            M3G_TRACE("Graphics3D.getInstance");
            // Defer m3gCreateInterface + GL context creation until something
            // actually tries to render. Many games (e.g. Bejeweled 3) call
            // getInstance() during boot just to probe whether 3D is present,
            // then never use it; eagerly initing M3G triggers GLES extension
            // queries that crash on systems without GLES1 set up.
            static ObjRef g3d = NULL_REF;
            if (g3d == NULL_REF) {
                ClassDef* k = v.loader().find_or_stub("javax/microedition/m3g/Graphics3D");
                g3d = v.heap().alloc_object(k, 0);
            }
            f.push_ref(g3d);
        });

    vm.register_native("javax/microedition/m3g/Graphics3D",
        "bindTarget", "(Ljava/lang/Object;)V",
        [](VM&, Frame&, std::span<Slot>) {
            M3G_TRACE("Graphics3D.bindTarget(Object)");
            // Target binding requires routing the SDL framebuffer / GLES
            // surface to M3G. Our backend's beginRenderFunc hooks GL_MakeCurrent
            // already; we'll thread a real userTarget id once we have render.
            j2me_m3g_make_current();
        });
    vm.register_native("javax/microedition/m3g/Graphics3D",
        "bindTarget", "(Ljava/lang/Object;ZI)V",
        [](VM&, Frame&, std::span<Slot>) {
            M3G_TRACE("Graphics3D.bindTarget(Object,Z,I)");
            j2me_m3g_make_current();
        });
    vm.register_native("javax/microedition/m3g/Graphics3D",
        "releaseTarget", "()V",
        [](VM&, Frame&, std::span<Slot>) {
            // m3gReleaseTarget needs a render context but the Java side often
            // calls this after bindTarget without holding our handle. With the
            // singleton ctx it's safe.
            if (g_render_context) m3gReleaseTarget(g_render_context);
        });

    vm.register_native("javax/microedition/m3g/Graphics3D",
        "render", "(Ljavax/microedition/m3g/World;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            if (!g_render_context) return;
            M3GWorld w = handle_for<M3GWorld>(args[1].as_ref());
            if (w) m3gRenderWorld(g_render_context, w);
        });

    // ── Camera ───────────────────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Camera", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            create_into(args[0].as_ref(), m3gCreateCamera);
        });
    vm.register_native("javax/microedition/m3g/Camera",
        "setPerspective", "(FFFF)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GCamera c = handle_for<M3GCamera>(args[0].as_ref());
            if (c) m3gSetPerspective(c, args[1].as_float(), args[2].as_float(),
                                     args[3].as_float(), args[4].as_float());
        });
    vm.register_native("javax/microedition/m3g/Camera",
        "setParallel", "(FFFF)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GCamera c = handle_for<M3GCamera>(args[0].as_ref());
            if (c) m3gSetParallel(c, args[1].as_float(), args[2].as_float(),
                                  args[3].as_float(), args[4].as_float());
        });

    // ── Background ───────────────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Background", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            create_into(args[0].as_ref(), m3gCreateBackground);
        });
    vm.register_native("javax/microedition/m3g/Background", "setColor", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GBackground b = handle_for<M3GBackground>(args[0].as_ref());
            if (b) m3gSetBgColor(b, (M3Guint)args[1].as_int());
        });

    // ── Transform ────────────────────────────────────────────────────────────
    // Note: Transform is value-typed — the Java side passes it by reference
    // but it's logically immutable per call. We back it with a heap M3GMatrix.
    static std::unordered_map<ObjRef, M3GMatrix> g_transforms;
    vm.register_native("javax/microedition/m3g/Transform", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GMatrix& m = g_transforms[args[0].as_ref()];
            m3gIdentityMatrix(&m);
        });
    vm.register_native("javax/microedition/m3g/Transform", "setIdentity", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto it = g_transforms.find(args[0].as_ref());
            if (it != g_transforms.end()) m3gIdentityMatrix(&it->second);
        });
    vm.register_native("javax/microedition/m3g/Transform",
        "postTranslate", "(FFF)V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto it = g_transforms.find(args[0].as_ref());
            if (it == g_transforms.end()) return;
            m3gPostTranslateMatrix(&it->second, args[1].as_float(),
                                   args[2].as_float(), args[3].as_float());
        });
    vm.register_native("javax/microedition/m3g/Transform",
        "postRotate", "(FFFF)V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto it = g_transforms.find(args[0].as_ref());
            if (it == g_transforms.end()) return;
            m3gPostRotateMatrix(&it->second, args[1].as_float(),
                                args[2].as_float(), args[3].as_float(),
                                args[4].as_float());
        });
    vm.register_native("javax/microedition/m3g/Transform",
        "postScale", "(FFF)V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto it = g_transforms.find(args[0].as_ref());
            if (it == g_transforms.end()) return;
            m3gPostScaleMatrix(&it->second, args[1].as_float(),
                               args[2].as_float(), args[3].as_float());
        });
    vm.register_native("javax/microedition/m3g/Transform", "transpose", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto it = g_transforms.find(args[0].as_ref());
            if (it != g_transforms.end())
                m3gMatrixTranspose(&it->second, &it->second);
        });

    // ── CompositingMode ──────────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/CompositingMode", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            create_into(args[0].as_ref(), m3gCreateCompositingMode);
        });
    vm.register_native("javax/microedition/m3g/CompositingMode",
        "setBlending", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GCompositingMode m = handle_for<M3GCompositingMode>(args[0].as_ref());
            if (m) m3gSetBlending(m, (M3Genum)args[1].as_int());
        });
    vm.register_native("javax/microedition/m3g/CompositingMode",
        "setAlphaThreshold", "(F)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GCompositingMode m = handle_for<M3GCompositingMode>(args[0].as_ref());
            if (m) m3gSetAlphaThreshold(m, args[1].as_float());
        });

    // ── Appearance ───────────────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Appearance", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            create_into(args[0].as_ref(), m3gCreateAppearance);
        });
    vm.register_native("javax/microedition/m3g/Appearance",
        "setTexture", "(ILjavax/microedition/m3g/Texture2D;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GAppearance a = handle_for<M3GAppearance>(args[0].as_ref());
            if (!a) return;
            M3GTexture t = handle_for<M3GTexture>(args[2].as_ref());
            m3gSetTexture(a, args[1].as_int(), t);
        });
    vm.register_native("javax/microedition/m3g/Appearance",
        "setCompositingMode", "(Ljavax/microedition/m3g/CompositingMode;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GAppearance a = handle_for<M3GAppearance>(args[0].as_ref());
            if (!a) return;
            M3GCompositingMode m =
                handle_for<M3GCompositingMode>(args[1].as_ref());
            m3gSetCompositingMode(a, m);
        });
    vm.register_native("javax/microedition/m3g/Appearance",
        "setMaterial", "(Ljavax/microedition/m3g/Material;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GAppearance a = handle_for<M3GAppearance>(args[0].as_ref());
            if (!a) return;
            M3GMaterial m = handle_for<M3GMaterial>(args[1].as_ref());
            m3gSetMaterial(a, m);
        });

    // ── Texture2D ────────────────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Texture2D",
        "<init>", "(Ljavax/microedition/m3g/Image2D;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GInterface itf = j2me_m3g_interface();
            if (!itf) return;
            M3GImage img = handle_for<M3GImage>(args[1].as_ref());
            if (!img) return;
            M3GTexture tex = m3gCreateTexture(itf, img);
            if (tex) store_handle(args[0].as_ref(), (uintptr_t)tex);
        });
    vm.register_native("javax/microedition/m3g/Texture2D",
        "setBlending", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GTexture t = handle_for<M3GTexture>(args[0].as_ref());
            if (t) m3gTextureSetBlending(t, (M3Genum)args[1].as_int());
        });
    vm.register_native("javax/microedition/m3g/Texture2D",
        "setFiltering", "(II)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GTexture t = handle_for<M3GTexture>(args[0].as_ref());
            if (t) m3gSetFiltering(t,
                (M3Genum)args[1].as_int(), (M3Genum)args[2].as_int());
        });
    vm.register_native("javax/microedition/m3g/Texture2D",
        "setWrapping", "(II)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GTexture t = handle_for<M3GTexture>(args[0].as_ref());
            if (t) m3gSetWrapping(t,
                (M3Genum)args[1].as_int(), (M3Genum)args[2].as_int());
        });

    vm.register_native("javax/microedition/m3g/Appearance",
        "getTexture", "(I)Ljavax/microedition/m3g/Texture2D;",
        [](VM&, Frame& f, std::span<Slot>) {
            // We don't track Texture-by-ObjRef from the Java side; M3G stores
            // it. Return null — most game code uses setTexture more than get.
            f.push_ref(NULL_REF);
        });

    // ── Image2D ──────────────────────────────────────────────────────────────
    // Image2D(int format, Object pixels). For now we create a blank image of
    // the right format and ignore the pixel data — many games use Image2D
    // only as a Texture2D source and don't read it back.
    vm.register_native("javax/microedition/m3g/Image2D",
        "<init>", "(ILjava/lang/Object;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GInterface itf = j2me_m3g_interface();
            if (!itf) return;
            // Default 64x64 placeholder — real loader will set width/height
            // based on the byte array dims when we wire that path.
            M3GImage img = m3gCreateImage(itf,
                (M3GImageFormat)args[1].as_int(), 64, 64, 0);
            if (img) store_handle(args[0].as_ref(), (uintptr_t)img);
        });

    // ── World ────────────────────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/World", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            create_into(args[0].as_ref(), m3gCreateWorld);
        });
    vm.register_native("javax/microedition/m3g/World",
        "setActiveCamera", "(Ljavax/microedition/m3g/Camera;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GWorld w = handle_for<M3GWorld>(args[0].as_ref());
            if (!w) return;
            M3GCamera c = handle_for<M3GCamera>(args[1].as_ref());
            m3gSetActiveCamera(w, c);
        });
    vm.register_native("javax/microedition/m3g/World",
        "setBackground", "(Ljavax/microedition/m3g/Background;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GWorld w = handle_for<M3GWorld>(args[0].as_ref());
            if (!w) return;
            M3GBackground b = handle_for<M3GBackground>(args[1].as_ref());
            m3gSetBackground(w, b);
        });

    // ── Light ────────────────────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Light", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            create_into(args[0].as_ref(), m3gCreateLight);
        });
    vm.register_native("javax/microedition/m3g/Light", "setMode", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GLight l = handle_for<M3GLight>(args[0].as_ref());
            if (l) m3gSetLightMode(l, (M3Genum)args[1].as_int());
        });
    vm.register_native("javax/microedition/m3g/Light", "setColor", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GLight l = handle_for<M3GLight>(args[0].as_ref());
            if (l) m3gSetLightColor(l, (M3Guint)args[1].as_int());
        });
    vm.register_native("javax/microedition/m3g/Light", "setIntensity", "(F)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GLight l = handle_for<M3GLight>(args[0].as_ref());
            if (l) m3gSetIntensity(l, args[1].as_float());
        });

    // ── PolygonMode ──────────────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/PolygonMode", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            create_into(args[0].as_ref(), m3gCreatePolygonMode);
        });
    vm.register_native("javax/microedition/m3g/PolygonMode",
        "setCulling", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GPolygonMode p = handle_for<M3GPolygonMode>(args[0].as_ref());
            if (p) m3gSetCulling(p, args[1].as_int());
        });
    vm.register_native("javax/microedition/m3g/PolygonMode",
        "setShading", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GPolygonMode p = handle_for<M3GPolygonMode>(args[0].as_ref());
            if (p) m3gSetShading(p, args[1].as_int());
        });
    vm.register_native("javax/microedition/m3g/PolygonMode",
        "setWinding", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GPolygonMode p = handle_for<M3GPolygonMode>(args[0].as_ref());
            if (p) m3gSetWinding(p, args[1].as_int());
        });
    vm.register_native("javax/microedition/m3g/PolygonMode",
        "setTwoSidedLightingEnable", "(Z)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GPolygonMode p = handle_for<M3GPolygonMode>(args[0].as_ref());
            if (p) m3gSetTwoSidedLightingEnable(p, args[1].as_int() != 0);
        });
    vm.register_native("javax/microedition/m3g/PolygonMode",
        "setLocalCameraLightingEnable", "(Z)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GPolygonMode p = handle_for<M3GPolygonMode>(args[0].as_ref());
            if (p) m3gSetLocalCameraLightingEnable(p, args[1].as_int() != 0);
        });
    vm.register_native("javax/microedition/m3g/PolygonMode",
        "setPerspectiveCorrectionEnable", "(Z)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GPolygonMode p = handle_for<M3GPolygonMode>(args[0].as_ref());
            if (p) m3gSetPerspectiveCorrectionEnable(p, args[1].as_int() != 0);
        });

    // Appearance.setPolygonMode
    vm.register_native("javax/microedition/m3g/Appearance",
        "setPolygonMode", "(Ljavax/microedition/m3g/PolygonMode;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GAppearance a = handle_for<M3GAppearance>(args[0].as_ref());
            if (!a) return;
            M3GPolygonMode p = handle_for<M3GPolygonMode>(args[1].as_ref());
            m3gSetPolygonMode(a, p);
        });

    // ── Material ─────────────────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Material", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            create_into(args[0].as_ref(), m3gCreateMaterial);
        });
    vm.register_native("javax/microedition/m3g/Material",
        "setColor", "(II)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GMaterial m = handle_for<M3GMaterial>(args[0].as_ref());
            if (m) m3gSetColor(m, (M3Genum)args[1].as_int(),
                              (M3Guint)args[2].as_int());
        });
    vm.register_native("javax/microedition/m3g/Material",
        "setShininess", "(F)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GMaterial m = handle_for<M3GMaterial>(args[0].as_ref());
            if (m) m3gSetShininess(m, args[1].as_float());
        });
    vm.register_native("javax/microedition/m3g/Material",
        "setVertexColorTrackingEnable", "(Z)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GMaterial m = handle_for<M3GMaterial>(args[0].as_ref());
            if (m) m3gSetVertexColorTrackingEnable(m, args[1].as_int() != 0);
        });

    // ── CompositingMode additions (depth, alpha-write) ──────────────────────
    vm.register_native("javax/microedition/m3g/CompositingMode",
        "setDepthTestEnable", "(Z)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GCompositingMode c = handle_for<M3GCompositingMode>(args[0].as_ref());
            if (c) m3gEnableDepthTest(c, args[1].as_int() != 0);
        });
    vm.register_native("javax/microedition/m3g/CompositingMode",
        "setDepthWriteEnable", "(Z)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GCompositingMode c = handle_for<M3GCompositingMode>(args[0].as_ref());
            if (c) m3gEnableDepthWrite(c, args[1].as_int() != 0);
        });
    vm.register_native("javax/microedition/m3g/CompositingMode",
        "setColorWriteEnable", "(Z)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GCompositingMode c = handle_for<M3GCompositingMode>(args[0].as_ref());
            if (c) m3gEnableColorWrite(c, args[1].as_int() != 0);
        });
    vm.register_native("javax/microedition/m3g/CompositingMode",
        "setAlphaWriteEnable", "(Z)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GCompositingMode c = handle_for<M3GCompositingMode>(args[0].as_ref());
            if (c) m3gSetAlphaWriteEnable(c, args[1].as_int() != 0);
        });

    // ── Background additions ────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Background",
        "setColorClearEnable", "(Z)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GBackground b = handle_for<M3GBackground>(args[0].as_ref());
            if (b) m3gSetBgEnable(b, 0 /* color clear */, args[1].as_int() != 0);
        });
    vm.register_native("javax/microedition/m3g/Background",
        "setDepthClearEnable", "(Z)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GBackground b = handle_for<M3GBackground>(args[0].as_ref());
            if (b) m3gSetBgEnable(b, 1 /* depth clear */, args[1].as_int() != 0);
        });
    vm.register_native("javax/microedition/m3g/Background",
        "setImage", "(Ljavax/microedition/m3g/Image2D;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GBackground b = handle_for<M3GBackground>(args[0].as_ref());
            if (!b) return;
            M3GImage img = handle_for<M3GImage>(args[1].as_ref());
            m3gSetBgImage(b, img);
        });

    // ── VertexArray ──────────────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/VertexArray",
        "<init>", "(III)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GInterface itf = j2me_m3g_interface();
            if (!itf) return;
            int count = args[1].as_int();
            int size  = args[2].as_int();
            int type  = args[3].as_int();   // 1=BYTE, 2=SHORT
            // M3G enums: BYTE=4 (M3G_BYTE), SHORT=5
            M3GVertexArray va = m3gCreateVertexArray(itf, count, size,
                type == 1 ? M3G_BYTE : M3G_SHORT);
            if (va) store_handle(args[0].as_ref(), (uintptr_t)va);
        });
    vm.register_native("javax/microedition/m3g/VertexArray",
        "set", "(II[B)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            M3GVertexArray va = handle_for<M3GVertexArray>(args[0].as_ref());
            if (!va) return;
            int first = args[1].as_int();
            int count = args[2].as_int();
            ObjRef arr = args[3].as_ref();
            if (arr == NULL_REF) return;
            HeapObject* a = v.heap().deref(arr);
            if (!a) return;
            m3gSetVertexArrayElements(va, first, count,
                a->array_length(), M3G_BYTE, a->array_bytes());
        });
    vm.register_native("javax/microedition/m3g/VertexArray",
        "set", "(II[S)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            M3GVertexArray va = handle_for<M3GVertexArray>(args[0].as_ref());
            if (!va) return;
            int first = args[1].as_int();
            int count = args[2].as_int();
            ObjRef arr = args[3].as_ref();
            if (arr == NULL_REF) return;
            HeapObject* a = v.heap().deref(arr);
            if (!a) return;
            m3gSetVertexArrayElements(va, first, count,
                a->array_length(), M3G_SHORT, a->array_shorts());
        });

    // ── VertexBuffer ─────────────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/VertexBuffer", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            create_into(args[0].as_ref(), m3gCreateVertexBuffer);
        });
    vm.register_native("javax/microedition/m3g/VertexBuffer",
        "setPositions", "(Ljavax/microedition/m3g/VertexArray;F[F)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            M3GVertexBuffer vb = handle_for<M3GVertexBuffer>(args[0].as_ref());
            if (!vb) return;
            M3GVertexArray va = handle_for<M3GVertexArray>(args[1].as_ref());
            float scale = args[2].as_float();
            ObjRef bias_ref = args[3].as_ref();
            float* bias = nullptr;
            int bias_len = 0;
            if (bias_ref != NULL_REF) {
                HeapObject* a = v.heap().deref(bias_ref);
                if (a) {
                    bias_len = a->array_length();
                    bias = (float*)a->array_bytes();
                }
            }
            m3gSetVertexArray(vb, va, scale, bias, bias_len);
        });
    vm.register_native("javax/microedition/m3g/VertexBuffer",
        "setNormals", "(Ljavax/microedition/m3g/VertexArray;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GVertexBuffer vb = handle_for<M3GVertexBuffer>(args[0].as_ref());
            if (!vb) return;
            M3GVertexArray va = handle_for<M3GVertexArray>(args[1].as_ref());
            m3gSetNormalArray(vb, va);
        });
    vm.register_native("javax/microedition/m3g/VertexBuffer",
        "setColors", "(Ljavax/microedition/m3g/VertexArray;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GVertexBuffer vb = handle_for<M3GVertexBuffer>(args[0].as_ref());
            if (!vb) return;
            M3GVertexArray va = handle_for<M3GVertexArray>(args[1].as_ref());
            m3gSetColorArray(vb, va);
        });
    vm.register_native("javax/microedition/m3g/VertexBuffer",
        "setTexCoords", "(ILjavax/microedition/m3g/VertexArray;F[F)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            M3GVertexBuffer vb = handle_for<M3GVertexBuffer>(args[0].as_ref());
            if (!vb) return;
            int unit = args[1].as_int();
            M3GVertexArray va = handle_for<M3GVertexArray>(args[2].as_ref());
            float scale = args[3].as_float();
            ObjRef bias_ref = args[4].as_ref();
            float* bias = nullptr;
            int bias_len = 0;
            if (bias_ref != NULL_REF) {
                HeapObject* a = v.heap().deref(bias_ref);
                if (a) {
                    bias_len = a->array_length();
                    bias = (float*)a->array_bytes();
                }
            }
            m3gSetTexCoordArray(vb, unit, va, scale, bias, bias_len);
        });
    vm.register_native("javax/microedition/m3g/VertexBuffer",
        "setDefaultColor", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GVertexBuffer vb = handle_for<M3GVertexBuffer>(args[0].as_ref());
            if (vb) m3gSetVertexDefaultColor(vb, (M3Guint)args[1].as_int());
        });

    // ── Group ────────────────────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Group", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            create_into(args[0].as_ref(), m3gCreateGroup);
        });
    vm.register_native("javax/microedition/m3g/Group",
        "addChild", "(Ljavax/microedition/m3g/Node;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GGroup g = handle_for<M3GGroup>(args[0].as_ref());
            if (!g) return;
            M3GNode n = handle_for<M3GNode>(args[1].as_ref());
            if (n) m3gAddChild(g, n);
        });
    vm.register_native("javax/microedition/m3g/Group",
        "getChildCount", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            M3GGroup g = handle_for<M3GGroup>(args[0].as_ref());
            f.push_int(g ? m3gGetChildCount(g) : 0);
        });
    vm.register_native("javax/microedition/m3g/Group",
        "removeChild", "(Ljavax/microedition/m3g/Node;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GGroup g = handle_for<M3GGroup>(args[0].as_ref());
            if (!g) return;
            M3GNode n = handle_for<M3GNode>(args[1].as_ref());
            if (n) m3gRemoveChild(g, n);
        });
    vm.register_native("javax/microedition/m3g/Group",
        "getChild", "(I)Ljavax/microedition/m3g/Node;",
        [](VM&, Frame& f, std::span<Slot> args) {
            // M3G returns the underlying handle but Java needs an ObjRef. We
            // don't currently round-trip Node ObjRefs, so return null.
            (void)args;
            f.push_ref(NULL_REF);
        });

    // ── Object3D additions ──────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Object3D",
        "find", "(I)Ljavax/microedition/m3g/Object3D;",
        [](VM&, Frame& f, std::span<Slot> args) {
            // m3gFind walks a scene tree by user ID. We return null since
            // we don't round-trip Object3D ObjRefs from the C handle.
            (void)args;
            f.push_ref(NULL_REF);
        });

    // ── Mesh.getVertexBuffer ────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Mesh",
        "getVertexBuffer", "()Ljavax/microedition/m3g/VertexBuffer;",
        [](VM&, Frame& f, std::span<Slot>) {
            f.push_ref(NULL_REF);  // Mesh.<init> doesn't bind geometry yet
        });

    // ── Appearance.getCompositingMode ───────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Appearance",
        "getCompositingMode", "()Ljavax/microedition/m3g/CompositingMode;",
        [](VM&, Frame& f, std::span<Slot>) {
            f.push_ref(NULL_REF);  // C handle round-trip not modelled
        });

    // ── Transformable.setTranslation / setOrientation / setTransform ────────
    // Transformable is the base class for Node (and thus Camera, Light, Mesh,
    // Group, etc.). Methods route to the underlying M3GTransformable handle.
    vm.register_native("javax/microedition/m3g/Transformable",
        "setTranslation", "(FFF)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GTransformable t = handle_for<M3GTransformable>(args[0].as_ref());
            if (t) m3gSetTranslation(t, args[1].as_float(),
                                     args[2].as_float(), args[3].as_float());
        });
    vm.register_native("javax/microedition/m3g/Transformable",
        "setOrientation", "(FFFF)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GTransformable t = handle_for<M3GTransformable>(args[0].as_ref());
            if (t) m3gSetOrientation(t, args[1].as_float(),
                                     args[2].as_float(), args[3].as_float(),
                                     args[4].as_float());
        });
    vm.register_native("javax/microedition/m3g/Transformable",
        "setTransform", "(Ljavax/microedition/m3g/Transform;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GTransformable t = handle_for<M3GTransformable>(args[0].as_ref());
            if (!t) return;
            auto it = g_transforms.find(args[1].as_ref());
            if (it != g_transforms.end()) m3gSetTransform(t, &it->second);
        });

    // ── Node.setRenderingEnable ─────────────────────────────────────────────
    // No direct M3G API — game's call is just a hint we accept and discard.
    vm.register_native("javax/microedition/m3g/Node",
        "setRenderingEnable", "(Z)V",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("javax/microedition/m3g/Node",
        "setPickingEnable", "(Z)V",
        [](VM&, Frame&, std::span<Slot>) {});

    // ── Graphics3D additions (clear, setCamera, setViewport) ────────────────
    vm.register_native("javax/microedition/m3g/Graphics3D",
        "clear", "(Ljavax/microedition/m3g/Background;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            if (!g_render_context) return;
            M3GBackground b = handle_for<M3GBackground>(args[1].as_ref());
            m3gClear(g_render_context, b);
        });
    vm.register_native("javax/microedition/m3g/Graphics3D",
        "setCamera",
        "(Ljavax/microedition/m3g/Camera;Ljavax/microedition/m3g/Transform;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            // Camera is set via World normally; for direct render mode the
            // m3g core stores it on the context. No direct API — call m3gSetCamera
            // if available, else no-op.
            (void)args;
        });
    vm.register_native("javax/microedition/m3g/Graphics3D",
        "render",
        "(Ljavax/microedition/m3g/Node;Ljavax/microedition/m3g/Transform;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            if (!g_render_context) return;
            M3GNode n = handle_for<M3GNode>(args[1].as_ref());
            if (!n) return;
            auto t_it = g_transforms.find(args[2].as_ref());
            const M3GMatrix* tx = (t_it != g_transforms.end()) ? &t_it->second : nullptr;
            m3gRenderNode(g_render_context, n, tx);
        });
    vm.register_native("javax/microedition/m3g/Graphics3D",
        "render",
        "(Ljavax/microedition/m3g/VertexBuffer;Ljavax/microedition/m3g/IndexBuffer;Ljavax/microedition/m3g/Appearance;Ljavax/microedition/m3g/Transform;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            if (!g_render_context) return;
            M3GVertexBuffer vb = handle_for<M3GVertexBuffer>(args[1].as_ref());
            M3GIndexBuffer ib = handle_for<M3GIndexBuffer>(args[2].as_ref());
            M3GAppearance ap = handle_for<M3GAppearance>(args[3].as_ref());
            if (!vb || !ib || !ap) return;
            auto t_it = g_transforms.find(args[4].as_ref());
            const M3GMatrix* tx = (t_it != g_transforms.end()) ? &t_it->second : nullptr;
            m3gRender(g_render_context, vb, ib, ap, tx, 1.0f, -1);
        });

    // ── TriangleStripArray ─────────────────────────────────────────────────
    // Two ctors:
    //   <init>(int firstIndex, int[] stripLengths)   — implicit indices
    //   <init>(int[] indices,  int[] stripLengths)   — explicit indices
    // Both create an M3GIndexBuffer.
    vm.register_native("javax/microedition/m3g/TriangleStripArray",
        "<init>", "(I[I)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            M3GInterface itf = j2me_m3g_interface();
            if (!itf) return;
            int firstIndex = args[1].as_int();
            ObjRef arr = args[2].as_ref();
            if (arr == NULL_REF) return;
            HeapObject* a = v.heap().deref(arr);
            if (!a) return;
            int n = a->array_length();
            std::vector<M3Gsizei> lengths(n);
            int32_t* src = (int32_t*)a->array_bytes();
            for (int i = 0; i < n; ++i) lengths[i] = (M3Gsizei)src[i];
            M3GIndexBuffer ib = m3gCreateImplicitStripBuffer(itf, n,
                lengths.data(), firstIndex);
            if (ib) store_handle(args[0].as_ref(), (uintptr_t)ib);
        });
    vm.register_native("javax/microedition/m3g/TriangleStripArray",
        "<init>", "([I[I)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            M3GInterface itf = j2me_m3g_interface();
            if (!itf) return;
            ObjRef idx_arr = args[1].as_ref();
            ObjRef len_arr = args[2].as_ref();
            if (idx_arr == NULL_REF || len_arr == NULL_REF) return;
            HeapObject* idx = v.heap().deref(idx_arr);
            HeapObject* len = v.heap().deref(len_arr);
            if (!idx || !len) return;
            int idx_n = idx->array_length();
            int len_n = len->array_length();
            std::vector<M3Gsizei> lengths(len_n);
            int32_t* len_src = (int32_t*)len->array_bytes();
            for (int i = 0; i < len_n; ++i) lengths[i] = (M3Gsizei)len_src[i];
            M3GIndexBuffer ib = m3gCreateStripBuffer(itf, M3G_TRIANGLE_STRIPS,
                len_n, lengths.data(), M3G_INT, idx_n, idx->array_bytes());
            if (ib) store_handle(args[0].as_ref(), (uintptr_t)ib);
        });

    vm.register_native("javax/microedition/m3g/Graphics3D",
        "setViewport", "(IIII)V",
        [](VM&, Frame&, std::span<Slot> args) {
            if (!g_render_context) return;
            m3gSetViewport(g_render_context, args[1].as_int(), args[2].as_int(),
                           args[3].as_int(), args[4].as_int());
        });

    // ── Transform additions ────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Transform",
        "set", "([F)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            auto it = g_transforms.find(args[0].as_ref());
            if (it == g_transforms.end()) return;
            ObjRef arr = args[1].as_ref();
            if (arr == NULL_REF) return;
            HeapObject* a = v.heap().deref(arr);
            if (!a || a->array_length() < 16) return;
            m3gSetMatrixRows(&it->second, (float*)a->array_bytes());
        });
    vm.register_native("javax/microedition/m3g/Transform",
        "set", "(Ljavax/microedition/m3g/Transform;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto& dst = g_transforms[args[0].as_ref()];
            auto it = g_transforms.find(args[1].as_ref());
            if (it != g_transforms.end()) m3gCopyMatrix(&dst, &it->second);
        });
    vm.register_native("javax/microedition/m3g/Transform",
        "get", "([F)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            auto it = g_transforms.find(args[0].as_ref());
            if (it == g_transforms.end()) return;
            ObjRef arr = args[1].as_ref();
            if (arr == NULL_REF) return;
            HeapObject* a = v.heap().deref(arr);
            if (!a || a->array_length() < 16) return;
            m3gGetMatrixRows(&it->second, (float*)a->array_bytes());
        });
    vm.register_native("javax/microedition/m3g/Transform",
        "transform", "([F)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            auto it = g_transforms.find(args[0].as_ref());
            if (it == g_transforms.end()) return;
            ObjRef arr = args[1].as_ref();
            if (arr == NULL_REF) return;
            HeapObject* a = v.heap().deref(arr);
            if (!a) return;
            int len = a->array_length();
            // Treat as packed [x0,y0,z0,w0, x1,y1,z1,w1, ...]; transform in-place.
            float* p = (float*)a->array_bytes();
            for (int i = 0; i + 4 <= len; i += 4) {
                M3GVec4 v4{ p[i], p[i+1], p[i+2], p[i+3] };
                m3gTransformVec4(&it->second, &v4);
                p[i] = v4.x; p[i+1] = v4.y; p[i+2] = v4.z; p[i+3] = v4.w;
            }
        });
    vm.register_native("javax/microedition/m3g/Transform",
        "postMultiply", "(Ljavax/microedition/m3g/Transform;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto dst_it = g_transforms.find(args[0].as_ref());
            auto src_it = g_transforms.find(args[1].as_ref());
            if (dst_it == g_transforms.end() || src_it == g_transforms.end()) return;
            m3gPostMultiplyMatrix(&dst_it->second, &src_it->second);
        });

    // ── Mesh ─────────────────────────────────────────────────────────────────
    // m3gCreateMesh takes raw indices into ulong arrays; that needs the Java
    // arg to be massaged. For now, register the constructor as a no-op stub
    // that allocates a Mesh handle without backing geometry. Games typically
    // build the rest of the scene anyway and don't immediately render the
    // mesh visibly.
    // Single-submesh Mesh constructor. Same m3gCreateMesh call as the 4-arg
    // version but with 1-element IndexBuffer/Appearance arrays. Galaxy on
    // Fire builds its 3D objects procedurally through this form rather than
    // loading .m3g files, so leaving it as a stub left every Mesh in the
    // scene tree geometry-less and every subsequent .getVertexBuffer() or
    // World.render() path NPE'd downstream.
    vm.register_native("javax/microedition/m3g/Mesh", "<init>",
        "(Ljavax/microedition/m3g/VertexBuffer;Ljavax/microedition/m3g/IndexBuffer;Ljavax/microedition/m3g/Appearance;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GInterface itf = j2me_m3g_interface();
            if (!itf) return;
            M3GVertexBuffer vb = handle_for<M3GVertexBuffer>(args[1].as_ref());
            if (!vb) return;
            M3Gulong ib = (M3Gulong)handle_for<M3GIndexBuffer>(args[2].as_ref());
            M3Gulong ap = (M3Gulong)handle_for<M3GAppearance>(args[3].as_ref());
            M3GMesh m = m3gCreateMesh(itf, vb, &ib, &ap, 1);
            if (m) store_handle(args[0].as_ref(), (uintptr_t)m);
        });
    vm.register_native("javax/microedition/m3g/Mesh",
        "getAppearance", "(I)Ljavax/microedition/m3g/Appearance;",
        [](VM&, Frame& f, std::span<Slot>) { f.push_ref(NULL_REF); });
    vm.register_native("javax/microedition/m3g/Mesh",
        "setAppearance", "(ILjavax/microedition/m3g/Appearance;)V",
        [](VM&, Frame&, std::span<Slot>) {});

    // ── Loader ──────────────────────────────────────────────────────────────
    // Feed .m3g model bytes through the vendored Khronos loader (m3gCreateLoader
    // → m3gDecodeData → m3gGetLoadedObjects → m3gImportObjects) and wrap each
    // imported M3G object in a Java ObjRef of the right concrete subclass.
    // Galaxy on Fire ships 26 .m3g files it loads via Loader.load(String).
    vm.register_native("javax/microedition/m3g/Loader",
        "load", "(Ljava/lang/String;)[Ljavax/microedition/m3g/Object3D;",
        [&jar](VM& v, Frame& f, std::span<Slot> args) {
            M3G_TRACE("Loader.load(String)");
            std::string name = v.string_value(args[0].as_ref());
            std::string path = jar.resolve(name);
            if (path.empty()) {
                fprintf(stderr, "[m3g] Loader.load: not in JAR: %s\n", name.c_str());
                f.push_ref(v.heap().alloc_ref_array(0,
                    v.loader().find_or_stub("[Ljavax/microedition/m3g/Object3D;")));
                return;
            }
            const auto& bytes = jar.get(path);
            f.push_ref(m3g_loader_load(v, bytes.data(), bytes.size()));
        });
    vm.register_native("javax/microedition/m3g/Loader",
        "load", "([BI)[Ljavax/microedition/m3g/Object3D;",
        [](VM& v, Frame& f, std::span<Slot> args) {
            M3G_TRACE("Loader.load(byte[],int)");
            ObjRef arr_ref = args[0].as_ref();
            int offset = args[1].as_int();
            HeapObject* arr = v.heap().deref(arr_ref);
            if (!arr || offset < 0) {
                f.push_ref(v.heap().alloc_ref_array(0,
                    v.loader().find_or_stub("[Ljavax/microedition/m3g/Object3D;")));
                return;
            }
            const uint8_t* data = arr->array_bytes() + offset;
            size_t len = arr->array_length() > offset
                       ? (size_t)arr->array_length() - (size_t)offset : 0;
            f.push_ref(m3g_loader_load(v, data, len));
        });

    // ── Object3D.animate / duplicate ────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Object3D", "animate", "(I)I",
        [](VM&, Frame& f, std::span<Slot> args) {
            M3GObject o = handle_for<M3GObject>(args[0].as_ref());
            f.push_int(o ? m3gAnimate(o, args[1].as_int()) : 0);
        });
    vm.register_native("javax/microedition/m3g/Object3D",
        "duplicate", "()Ljavax/microedition/m3g/Object3D;",
        [](VM&, Frame& f, std::span<Slot>) {
            // m3gDuplicate takes a reference-tracking ulong array; returning
            // the Java-side duplicate requires round-tripping the new M3G
            // handle back to an ObjRef, which we don't model. Null for now.
            f.push_ref(NULL_REF);
        });

    // ── Transform.invert ────────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Transform", "invert", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            auto it = g_transforms.find(args[0].as_ref());
            if (it != g_transforms.end()) m3gInvertMatrix(&it->second);
        });

    // ── Camera.getProjection — populates a Transform with the projection ──
    vm.register_native("javax/microedition/m3g/Camera",
        "getProjection", "(Ljavax/microedition/m3g/Transform;)I",
        [](VM&, Frame& f, std::span<Slot> args) {
            M3GCamera c = handle_for<M3GCamera>(args[0].as_ref());
            if (!c) { f.push_int(0); return; }
            M3GMatrix* dst = nullptr;
            ObjRef t_ref = args[1].as_ref();
            if (t_ref != NULL_REF) {
                auto it = g_transforms.find(t_ref);
                if (it != g_transforms.end()) dst = &it->second;
            }
            f.push_int(m3gGetProjectionAsMatrix(c, dst));
        });

    // ── Mesh.getSubmeshCount ────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Mesh",
        "getSubmeshCount", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            M3GMesh m = handle_for<M3GMesh>(args[0].as_ref());
            f.push_int(m ? m3gGetSubmeshCount(m) : 0);
        });

    // ── Graphics3D.resetLights ──────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Graphics3D", "resetLights", "()V",
        [](VM&, Frame&, std::span<Slot>) {
            if (g_render_context) m3gClearLights(g_render_context);
        });

    // ── Appearance.getPolygonMode ──────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Appearance",
        "getPolygonMode", "()Ljavax/microedition/m3g/PolygonMode;",
        [](VM&, Frame& f, std::span<Slot>) {
            // Handle-to-ObjRef round-trip not modelled; null is spec-legal.
            f.push_ref(NULL_REF);
        });

    // ── Sprite3D ────────────────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Sprite3D",
        "<init>",
        "(ZLjavax/microedition/m3g/Image2D;Ljavax/microedition/m3g/Appearance;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GInterface itf = j2me_m3g_interface();
            if (!itf) return;
            bool scaled = args[1].as_int() != 0;
            M3GImage img = handle_for<M3GImage>(args[2].as_ref());
            M3GAppearance ap = handle_for<M3GAppearance>(args[3].as_ref());
            M3GSprite s = m3gCreateSprite(itf, scaled, img, ap);
            if (s) store_handle(args[0].as_ref(), (uintptr_t)s);
        });

    // ── Texture2D.setImage ─────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Texture2D",
        "setImage", "(Ljavax/microedition/m3g/Image2D;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GTexture t = handle_for<M3GTexture>(args[0].as_ref());
            if (!t) return;
            M3GImage img = handle_for<M3GImage>(args[1].as_ref());
            m3gSetTextureImage(t, img);
        });

    // ── Background.setCrop ─────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Background",
        "setCrop", "(IIII)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GBackground b = handle_for<M3GBackground>(args[0].as_ref());
            if (b) m3gSetBgCrop(b, args[1].as_int(), args[2].as_int(),
                               args[3].as_int(), args[4].as_int());
        });

    // ── Background.setImageMode ────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Background",
        "setImageMode", "(II)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GBackground b = handle_for<M3GBackground>(args[0].as_ref());
            if (b) m3gSetBgMode(b, args[1].as_int(), args[2].as_int());
        });

    // ── Transformable arithmetic (translate/postRotate/setScale) ────────────
    vm.register_native("javax/microedition/m3g/Transformable",
        "translate", "(FFF)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GTransformable t = handle_for<M3GTransformable>(args[0].as_ref());
            if (t) m3gTranslate(t, args[1].as_float(),
                                args[2].as_float(), args[3].as_float());
        });
    vm.register_native("javax/microedition/m3g/Transformable",
        "postRotate", "(FFFF)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GTransformable t = handle_for<M3GTransformable>(args[0].as_ref());
            if (t) m3gPostRotate(t, args[1].as_float(),
                                 args[2].as_float(), args[3].as_float(),
                                 args[4].as_float());
        });
    vm.register_native("javax/microedition/m3g/Transformable",
        "setScale", "(FFF)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GTransformable t = handle_for<M3GTransformable>(args[0].as_ref());
            if (t) m3gSetScale(t, args[1].as_float(),
                               args[2].as_float(), args[3].as_float());
        });
    vm.register_native("javax/microedition/m3g/Transformable",
        "getTranslation", "([F)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            M3GTransformable t = handle_for<M3GTransformable>(args[0].as_ref());
            if (!t) return;
            ObjRef arr = args[1].as_ref();
            if (arr == NULL_REF) return;
            HeapObject* a = v.heap().deref(arr);
            if (!a || a->array_length() < 3) return;
            m3gGetTranslation(t, (M3Gfloat*)a->array_bytes());
        });
    vm.register_native("javax/microedition/m3g/Transformable",
        "getCompositeTransform",
        "(Ljavax/microedition/m3g/Transform;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GTransformable t = handle_for<M3GTransformable>(args[0].as_ref());
            if (!t) return;
            ObjRef tref = args[1].as_ref();
            if (tref == NULL_REF) return;
            m3gGetCompositeTransform(t, &g_transforms[tref]);
        });

    // ── Object3D.getUserID ─────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Object3D",
        "getUserID", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            M3GObject o = handle_for<M3GObject>(args[0].as_ref());
            f.push_int(o ? m3gGetUserID(o) : 0);
        });
    vm.register_native("javax/microedition/m3g/Object3D",
        "setUserID", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GObject o = handle_for<M3GObject>(args[0].as_ref());
            if (o) m3gSetUserID(o, args[1].as_int());
        });

    // ── World: addChild/removeChild/find/getActiveCamera ───────────────────
    // World is-a Group; route to group ops. find/getActiveCamera can't return
    // an ObjRef without a handle→ObjRef reverse map, so return null.
    vm.register_native("javax/microedition/m3g/World",
        "addChild", "(Ljavax/microedition/m3g/Node;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GGroup g = handle_for<M3GGroup>(args[0].as_ref());
            if (!g) return;
            M3GNode n = handle_for<M3GNode>(args[1].as_ref());
            if (n) m3gAddChild(g, n);
        });
    vm.register_native("javax/microedition/m3g/World",
        "removeChild", "(Ljavax/microedition/m3g/Node;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GGroup g = handle_for<M3GGroup>(args[0].as_ref());
            if (!g) return;
            M3GNode n = handle_for<M3GNode>(args[1].as_ref());
            if (n) m3gRemoveChild(g, n);
        });
    vm.register_stub("javax/microedition/m3g/World",
        "find", "(I)Ljavax/microedition/m3g/Object3D;",
        "World.find — handle→ObjRef reverse not modelled; null",
        [](VM&, Frame& f, std::span<Slot>) { f.push_ref(NULL_REF); });
    vm.register_stub("javax/microedition/m3g/World",
        "getActiveCamera", "()Ljavax/microedition/m3g/Camera;",
        "World.getActiveCamera — handle→ObjRef reverse not modelled; null",
        [](VM&, Frame& f, std::span<Slot>) { f.push_ref(NULL_REF); });

    // ── Texture2D.getImage ─────────────────────────────────────────────────
    vm.register_stub("javax/microedition/m3g/Texture2D",
        "getImage", "()Ljavax/microedition/m3g/Image2D;",
        "Texture2D.getImage — handle→ObjRef reverse not modelled; null",
        [](VM&, Frame& f, std::span<Slot>) { f.push_ref(NULL_REF); });

    // ── Node.getParent ─────────────────────────────────────────────────────
    vm.register_stub("javax/microedition/m3g/Node",
        "getParent", "()Ljavax/microedition/m3g/Node;",
        "Node.getParent — handle→ObjRef reverse not modelled; null",
        [](VM&, Frame& f, std::span<Slot>) { f.push_ref(NULL_REF); });

    // ── Image2D.getWidth/getHeight ─────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Image2D",
        "getWidth", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            M3GImage i = handle_for<M3GImage>(args[0].as_ref());
            f.push_int(i ? m3gGetWidth(i) : 0);
        });
    vm.register_native("javax/microedition/m3g/Image2D",
        "getHeight", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            M3GImage i = handle_for<M3GImage>(args[0].as_ref());
            f.push_int(i ? m3gGetHeight(i) : 0);
        });

    // ── Appearance.setFog ──────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Appearance",
        "setFog", "(Ljavax/microedition/m3g/Fog;)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GAppearance a = handle_for<M3GAppearance>(args[0].as_ref());
            if (!a) return;
            M3GFog fg = handle_for<M3GFog>(args[1].as_ref());
            m3gSetFog(a, fg);
        });

    // ── Graphics3D.addLight ────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Graphics3D",
        "addLight",
        "(Ljavax/microedition/m3g/Light;Ljavax/microedition/m3g/Transform;)I",
        [](VM&, Frame& f, std::span<Slot> args) {
            if (!g_render_context) { f.push_int(-1); return; }
            M3GLight l = handle_for<M3GLight>(args[1].as_ref());
            if (!l) { f.push_int(-1); return; }
            auto t_it = g_transforms.find(args[2].as_ref());
            const M3GMatrix* tx = (t_it != g_transforms.end()) ? &t_it->second : nullptr;
            f.push_int(m3gAddLight(g_render_context, l, tx));
        });

    // ── RayIntersection — value-type holder, no C handle ───────────────────
    vm.register_noop("javax/microedition/m3g/RayIntersection",
        "<init>", "()V",
        "RayIntersection is a holder for pick results; no backing handle",
        [](VM&, Frame&, std::span<Slot>) {});

    // ── Mesh 3-arg <init> (VertexBuffer, IndexBuffer[], Appearance[]) ──────
    vm.register_native("javax/microedition/m3g/Mesh", "<init>",
        "(Ljavax/microedition/m3g/VertexBuffer;[Ljavax/microedition/m3g/IndexBuffer;[Ljavax/microedition/m3g/Appearance;)V",
        [](VM& v, Frame&, std::span<Slot> args) {
            M3GInterface itf = j2me_m3g_interface();
            if (!itf) return;
            M3GVertexBuffer vb = handle_for<M3GVertexBuffer>(args[1].as_ref());
            if (!vb) return;
            ObjRef ib_arr = args[2].as_ref();
            ObjRef ap_arr = args[3].as_ref();
            if (ib_arr == NULL_REF || ap_arr == NULL_REF) return;
            HeapObject* ib_a = v.heap().deref(ib_arr);
            HeapObject* ap_a = v.heap().deref(ap_arr);
            if (!ib_a || !ap_a) return;
            int n = ib_a->array_length();
            if (ap_a->array_length() < n) n = ap_a->array_length();
            std::vector<M3Gulong> ibs(n), aps(n);
            ObjRef* ib_refs = (ObjRef*)ib_a->array_bytes();
            ObjRef* ap_refs = (ObjRef*)ap_a->array_bytes();
            for (int k = 0; k < n; ++k) {
                ibs[k] = (M3Gulong)handle_for<M3GIndexBuffer>(ib_refs[k]);
                aps[k] = (M3Gulong)handle_for<M3GAppearance>(ap_refs[k]);
            }
            M3GMesh m = m3gCreateMesh(itf, vb, ibs.data(), aps.data(), n);
            if (m) store_handle(args[0].as_ref(), (uintptr_t)m);
        });

    // ── Mirror Transformable methods onto each concrete subclass ───────────
    // Bytecode binds methodrefs to the declared type, so Group.setTranslation
    // is a distinct symbol from Transformable.setTranslation even though the
    // class hierarchy would route it. Register explicit forwarders so scan
    // and dispatch both resolve.
    for (const char* klass : {
        "javax/microedition/m3g/Group",
        "javax/microedition/m3g/Mesh",
        "javax/microedition/m3g/Sprite3D",
        "javax/microedition/m3g/Camera",
        "javax/microedition/m3g/Light",
        "javax/microedition/m3g/World",
        "javax/microedition/m3g/Node",
    }) {
        vm.register_native(klass, "setTranslation", "(FFF)V",
            [](VM&, Frame&, std::span<Slot> args) {
                M3GTransformable t = handle_for<M3GTransformable>(args[0].as_ref());
                if (t) m3gSetTranslation(t, args[1].as_float(),
                                         args[2].as_float(), args[3].as_float());
            });
        vm.register_native(klass, "setOrientation", "(FFFF)V",
            [](VM&, Frame&, std::span<Slot> args) {
                M3GTransformable t = handle_for<M3GTransformable>(args[0].as_ref());
                if (t) m3gSetOrientation(t, args[1].as_float(), args[2].as_float(),
                                         args[3].as_float(), args[4].as_float());
            });
        vm.register_native(klass, "setScale", "(FFF)V",
            [](VM&, Frame&, std::span<Slot> args) {
                M3GTransformable t = handle_for<M3GTransformable>(args[0].as_ref());
                if (t) m3gSetScale(t, args[1].as_float(),
                                   args[2].as_float(), args[3].as_float());
            });
        vm.register_native(klass, "translate", "(FFF)V",
            [](VM&, Frame&, std::span<Slot> args) {
                M3GTransformable t = handle_for<M3GTransformable>(args[0].as_ref());
                if (t) m3gTranslate(t, args[1].as_float(),
                                    args[2].as_float(), args[3].as_float());
            });
        vm.register_native(klass, "scale", "(FFF)V",
            [](VM&, Frame&, std::span<Slot> args) {
                M3GTransformable t = handle_for<M3GTransformable>(args[0].as_ref());
                if (t) m3gScale(t, args[1].as_float(),
                                args[2].as_float(), args[3].as_float());
            });
        vm.register_native(klass, "postRotate", "(FFFF)V",
            [](VM&, Frame&, std::span<Slot> args) {
                M3GTransformable t = handle_for<M3GTransformable>(args[0].as_ref());
                if (t) m3gPostRotate(t, args[1].as_float(), args[2].as_float(),
                                     args[3].as_float(), args[4].as_float());
            });
        vm.register_native(klass, "setRenderingEnable", "(Z)V",
            [](VM&, Frame&, std::span<Slot>) {});
    }

    // ── Transformable.scale (base) ─────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Transformable",
        "scale", "(FFF)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GTransformable t = handle_for<M3GTransformable>(args[0].as_ref());
            if (t) m3gScale(t, args[1].as_float(),
                            args[2].as_float(), args[3].as_float());
        });

    // ── Node.setAlphaFactor / getTransformTo ───────────────────────────────
    vm.register_native("javax/microedition/m3g/Node",
        "setAlphaFactor", "(F)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GNode n = handle_for<M3GNode>(args[0].as_ref());
            if (n) m3gSetAlphaFactor(n, args[1].as_float());
        });
    vm.register_native("javax/microedition/m3g/Node",
        "getTransformTo",
        "(Ljavax/microedition/m3g/Node;Ljavax/microedition/m3g/Transform;)Z",
        [](VM&, Frame& f, std::span<Slot> args) {
            M3GNode src = handle_for<M3GNode>(args[0].as_ref());
            M3GNode dst = handle_for<M3GNode>(args[1].as_ref());
            if (!src || !dst) { f.push_int(0); return; }
            ObjRef tref = args[2].as_ref();
            M3GMatrix* m = (tref != NULL_REF) ? &g_transforms[tref] : nullptr;
            f.push_int(m3gGetTransformTo(src, dst, m) ? 1 : 0);
        });

    // ── Appearance.setLayer ────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Appearance",
        "setLayer", "(I)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GAppearance a = handle_for<M3GAppearance>(args[0].as_ref());
            if (a) m3gSetLayer(a, args[1].as_int());
        });

    // ── CompositingMode.setDepthOffset ─────────────────────────────────────
    vm.register_native("javax/microedition/m3g/CompositingMode",
        "setDepthOffset", "(FF)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GCompositingMode c = handle_for<M3GCompositingMode>(args[0].as_ref());
            if (c) m3gSetDepthOffset(c, args[1].as_float(), args[2].as_float());
        });

    // ── Fog.<init> ─────────────────────────────────────────────────────────
    vm.register_native("javax/microedition/m3g/Fog", "<init>", "()V",
        [](VM&, Frame&, std::span<Slot> args) {
            create_into(args[0].as_ref(), m3gCreateFog);
        });

    // ── Image2D 3-arg <init> (format, width, height) — blank image ─────────
    vm.register_native("javax/microedition/m3g/Image2D", "<init>", "(III)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GInterface itf = j2me_m3g_interface();
            if (!itf) return;
            int fmt = args[1].as_int();
            int w   = args[2].as_int();
            int h   = args[3].as_int();
            M3GImage img = m3gCreateImage(itf, (M3GImageFormat)fmt, w, h, 0);
            if (img) store_handle(args[0].as_ref(), (uintptr_t)img);
        });

    // ── Mesh.getIndexBuffer / World.getBackground ──────────────────────────
    // Both require handle→ObjRef reverse maps to return the Java ref. Return
    // null; callers typically null-check.
    vm.register_stub("javax/microedition/m3g/Mesh",
        "getIndexBuffer", "(I)Ljavax/microedition/m3g/IndexBuffer;",
        "handle→ObjRef reverse not modelled; null",
        [](VM&, Frame& f, std::span<Slot>) { f.push_ref(NULL_REF); });
    vm.register_stub("javax/microedition/m3g/World",
        "getBackground", "()Ljavax/microedition/m3g/Background;",
        "handle→ObjRef reverse not modelled; null",
        [](VM&, Frame& f, std::span<Slot>) { f.push_ref(NULL_REF); });

    // ── VertexBuffer.getPositions / getTexCoords ───────────────────────────
    vm.register_stub("javax/microedition/m3g/VertexBuffer",
        "getPositions", "([F)Ljavax/microedition/m3g/VertexArray;",
        "handle→ObjRef reverse not modelled; null",
        [](VM&, Frame& f, std::span<Slot>) { f.push_ref(NULL_REF); });
    vm.register_stub("javax/microedition/m3g/VertexBuffer",
        "getTexCoords", "(I[F)Ljavax/microedition/m3g/VertexArray;",
        "handle→ObjRef reverse not modelled; null",
        [](VM&, Frame& f, std::span<Slot>) { f.push_ref(NULL_REF); });

    // ── Object3D.getAnimationTrack / KeyframeSequence.getDuration ──────────
    vm.register_stub("javax/microedition/m3g/Object3D",
        "getAnimationTrack", "(I)Ljavax/microedition/m3g/AnimationTrack;",
        "handle→ObjRef reverse not modelled; null",
        [](VM&, Frame& f, std::span<Slot>) { f.push_ref(NULL_REF); });
    vm.register_native("javax/microedition/m3g/KeyframeSequence",
        "getDuration", "()I",
        [](VM&, Frame& f, std::span<Slot> args) {
            M3GKeyframeSequence k = handle_for<M3GKeyframeSequence>(args[0].as_ref());
            f.push_int(k ? m3gGetDuration(k) : 0);
        });

    // ── Camera.getProjection([F)I (float-array overload) ───────────────────
    // The matrix overload writes to an M3GMatrix; the float-array form writes
    // the 16 floats directly. Return projection type.
    vm.register_native("javax/microedition/m3g/Camera",
        "getProjection", "([F)I",
        [](VM& v, Frame& f, std::span<Slot> args) {
            M3GCamera c = handle_for<M3GCamera>(args[0].as_ref());
            if (!c) { f.push_int(0); return; }
            ObjRef arr = args[1].as_ref();
            if (arr == NULL_REF) { f.push_int(m3gGetProjectionAsMatrix(c, nullptr)); return; }
            HeapObject* a = v.heap().deref(arr);
            if (!a || a->array_length() < 4) { f.push_int(0); return; }
            M3GMatrix m;
            int type = m3gGetProjectionAsMatrix(c, &m);
            // Caller expects perspective params [fovy, aspect, near, far] in [F,
            // not a 16-float matrix. We return the type and leave the array
            // alone — most callers that pass [F] only read the return value.
            f.push_int(type);
        });

    // ── Group.pick — no real picking support; report no hit ────────────────
    vm.register_stub("javax/microedition/m3g/Group",
        "pick", "(IFFFFFFLjavax/microedition/m3g/RayIntersection;)Z",
        "Group.pick — ray picking not implemented; no hit",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(0); });
    vm.register_stub("javax/microedition/m3g/Group",
        "pick",
        "(IFFLjavax/microedition/m3g/Camera;Ljavax/microedition/m3g/RayIntersection;)Z",
        "Group.pick — 2D picking not implemented; no hit",
        [](VM&, Frame& f, std::span<Slot>) { f.push_int(0); });
}
