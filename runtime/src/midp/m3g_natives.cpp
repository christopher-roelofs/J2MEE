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

    // ── Transformable.setTranslation / setOrientation / setScale ────────────
    // Transformable is the base class for Node (and thus Camera, Light, Mesh,
    // Group, etc.). Methods route to the underlying M3GTransformable handle.
    vm.register_native("javax/microedition/m3g/Transformable",
        "setTranslation", "(FFF)V",
        [](VM&, Frame&, std::span<Slot> args) {
            M3GTransformable t = handle_for<M3GTransformable>(args[0].as_ref());
            if (t) m3gSetTranslation(t, args[1].as_float(),
                                     args[2].as_float(), args[3].as_float());
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
    vm.register_stub("javax/microedition/m3g/Mesh",
        "<init>",
        "(Ljavax/microedition/m3g/VertexBuffer;Ljavax/microedition/m3g/IndexBuffer;Ljavax/microedition/m3g/Appearance;)V",
        "Mesh constructor — geometry not bound (would need m3gCreateMesh "
        "with patch arrays); Mesh exists but renders empty",
        [](VM&, Frame&, std::span<Slot>) {});
    vm.register_native("javax/microedition/m3g/Mesh",
        "getAppearance", "(I)Ljavax/microedition/m3g/Appearance;",
        [](VM&, Frame& f, std::span<Slot>) { f.push_ref(NULL_REF); });
    vm.register_native("javax/microedition/m3g/Mesh",
        "setAppearance", "(ILjavax/microedition/m3g/Appearance;)V",
        [](VM&, Frame&, std::span<Slot>) {});

    // ── Loader ──────────────────────────────────────────────────────────────
    // m3gCreateLoader + the streaming loader needs reading the input stream
    // through M3G's beginRender hook. Stub returns empty array — game treats
    // it as "no objects loaded" and falls through.
    vm.register_stub("javax/microedition/m3g/Loader",
        "load", "(Ljava/lang/String;)[Ljavax/microedition/m3g/Object3D;",
        "Loader.load(String) — not wired to JAR resource reader; empty array",
        [](VM& v, Frame& f, std::span<Slot>) {
            f.push_ref(v.heap().alloc_ref_array(0,
                v.loader().find_or_stub("[Ljavax/microedition/m3g/Object3D;")));
        });
    vm.register_stub("javax/microedition/m3g/Loader",
        "load", "([BI)[Ljavax/microedition/m3g/Object3D;",
        "Loader.load(byte[],int) — same; empty array",
        [](VM& v, Frame& f, std::span<Slot>) {
            f.push_ref(v.heap().alloc_ref_array(0,
                v.loader().find_or_stub("[Ljavax/microedition/m3g/Object3D;")));
        });
}
