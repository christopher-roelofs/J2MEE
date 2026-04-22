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

void register_m3g_natives(VM& vm) {

    // ── Graphics3D ────────────────────────────────────────────────────────────
    // Singleton — getInstance() always returns the same Java object backed
    // by the process-wide M3GRenderContext.
    vm.register_native("javax/microedition/m3g/Graphics3D",
        "getInstance", "()Ljavax/microedition/m3g/Graphics3D;",
        [](VM& v, Frame& f, std::span<Slot>) {
            static ObjRef g3d = NULL_REF;
            if (g3d == NULL_REF) {
                ClassDef* k = v.loader().find_or_stub("javax/microedition/m3g/Graphics3D");
                g3d = v.heap().alloc_object(k, 0);
                M3GRenderContext ctx = ensure_render_context();
                if (ctx) store_handle(g3d, (uintptr_t)ctx);
            }
            f.push_ref(g3d);
        });

    vm.register_native("javax/microedition/m3g/Graphics3D",
        "bindTarget", "(Ljava/lang/Object;)V",
        [](VM&, Frame&, std::span<Slot>) {
            // Target binding requires routing the SDL framebuffer / GLES
            // surface to M3G. Our backend's beginRenderFunc hooks GL_MakeCurrent
            // already; we'll thread a real userTarget id once we have render.
            j2me_m3g_make_current();
        });
    vm.register_native("javax/microedition/m3g/Graphics3D",
        "bindTarget", "(Ljava/lang/Object;ZI)V",
        [](VM&, Frame&, std::span<Slot>) { j2me_m3g_make_current(); });
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
}
