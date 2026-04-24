#include <csignal>
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>
#include "util/jar.hpp"
#include "util/game_config.hpp"
#include "vm/vm.hpp"
#include "vm/stub_registry.hpp"
#include "midp/natives.hpp"
#include "backend/display.hpp"
#include <filesystem>
namespace fs = std::filesystem;

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#ifdef __ANDROID__
extern "C" int j2me_android_bootstrap(const char* version_tag,
                                      std::vector<std::string>& out_argv);
#endif

// Declared in interpreter.cpp — set true to trace every opcode
extern bool g_trace;

// Defined in graphics_natives.cpp — override before register_graphics_natives
extern int g_screen_w, g_screen_h;

namespace {

// SIGINT (Ctrl+C) / SIGTERM handler. Does nothing beyond setting a flag and
// printing a short note — signal handlers must be async-signal-safe, and
// write(2) is, fprintf is not. Display::flush() polls the flag and returns
// false next frame, which throws QuitRequest and unwinds cleanly so RMS
// saves and other finalizers run. A second signal triggers immediate exit.
void on_quit_signal(int) {
    if (g_quit_requested) {
        static const char kMsg[] = "\n[j2me] second signal — forcing exit\n";
        (void)!write(2, kMsg, sizeof(kMsg) - 1);
        _exit(130);
    }
    g_quit_requested = 1;
    static const char kMsg[] = "\n[j2me] shutdown requested — unwinding\n";
    (void)!write(2, kMsg, sizeof(kMsg) - 1);
}

void install_signal_handlers() {
    struct sigaction sa{};
    sa.sa_handler = on_quit_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // no SA_RESTART — we want blocking syscalls to wake
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

void print_usage(const char* argv0) {
    std::cerr <<
        "usage: " << argv0 << " <file.jar> <MIDletClass> [WxH] [options]\n"
        "\n"
        "Automation / headless:\n"
        "  --headless           No SDL window. Games render to an offscreen\n"
        "                       surface; no user input is polled.\n"
        "  --ticks N            Max flush() calls before forcing quit.\n"
        "  --tick-ms M          Delay (ms) per flush in headless (default 0).\n"
        "  --run-ms N           Wall-clock cap (ms) from first flush.\n"
        "  --ppm PATH           Write framebuffer as P6 on exit.\n"
        "  --keys LIST          Comma-separated scripted keys. Names:\n"
        "                       UP DOWN LEFT RIGHT FIRE SELECT SOFT1 SOFT2\n"
        "                       STAR POUND  0..9  (digit names = digit key)\n"
        "                       Each key gets a press/release pair spaced\n"
        "                       J2ME_KEY_INTERVAL ticks apart (default 15)\n"
        "                       and held J2ME_KEY_HOLD ticks (default 3).\n"
        "  --quiet              Silence startup banner.\n"
        "  --bios PATH          Optional classes.jar with MIDP/CLDC framework\n"
        "                       impls (e.g. compiled phoneME sources). Game\n"
        "                       jar shadows BIOS on name collision.\n"
        "  --auto-res           Try to detect target screen resolution from\n"
        "                       manifest hints + PNG-mode dimensions. Off by\n"
        "                       default — heuristic, sometimes wrong; pass\n"
        "                       an explicit WxH for guaranteed-correct sizing.\n";
}

int avk_from_name(const std::string& s) {
    // Digits — numeric name maps to MIDP ASCII code.
    if (s.size() == 1 && s[0] >= '0' && s[0] <= '9') return (int)s[0];
    if (s == "UP")     return -1;
    if (s == "DOWN")   return -2;
    if (s == "LEFT")   return -3;
    if (s == "RIGHT")  return -4;
    if (s == "FIRE" || s == "SELECT" || s == "OK") return -5;
    if (s == "SOFT1" || s == "LSOFT" || s == "LEFTSOFT")  return -6;
    if (s == "SOFT2" || s == "RSOFT" || s == "RIGHTSOFT") return -7;
    if (s == "STAR")   return 42;   // '*'
    if (s == "POUND" || s == "HASH") return 35;  // '#'
    return 0;
}

std::vector<ScriptedKey> build_key_script(const std::string& list) {
    std::vector<ScriptedKey> out;
    int interval = 15, hold = 3;
    if (const char* e = std::getenv("J2ME_KEY_INTERVAL")) interval = std::max(1, std::atoi(e));
    if (const char* e = std::getenv("J2ME_KEY_HOLD"))     hold     = std::max(1, std::atoi(e));
    size_t start = 0;
    int idx = 0;
    while (start <= list.size()) {
        size_t c = list.find(',', start);
        if (c == std::string::npos) c = list.size();
        if (c > start) {
            std::string tok = list.substr(start, c - start);
            while (!tok.empty() && (tok.front() == ' ' || tok.front() == '\t')) tok.erase(tok.begin());
            while (!tok.empty() && (tok.back()  == ' ' || tok.back()  == '\t')) tok.pop_back();
            int code = avk_from_name(tok);
            if (code) {
                out.push_back({code, idx * interval, hold});
                ++idx;
            }
        }
        start = c + 1;
    }
    return out;
}

} // namespace

int main(int argc, char* argv[]) {
    g_trace = (std::getenv("J2ME_TRACE") != nullptr);
    extern uint64_t g_heartbeat_insns;
    if (const char* e = std::getenv("J2ME_HEARTBEAT")) {
        g_heartbeat_insns = (uint64_t)std::strtoull(e, nullptr, 10);
    }
    install_signal_handlers();

#ifdef __EMSCRIPTEN__
    // Browser entry: hardcode the POC invocation. The JAR and resolution come
    // from CMake cache variables baked into the build (J2ME_WEB_MIDLET,
    // J2ME_WEB_RES). Everything is preloaded into MEMFS via --preload-file.
    // Ignore whatever argv the JS glue synthesized.
    static const char* kArgv[] = {
        "j2me",
        "/game.jar",
        J2ME_WEB_MIDLET,
        J2ME_WEB_RES,
        nullptr,
    };
    argc = 4;
    argv = const_cast<char**>(kArgv);
#endif

#ifdef __ANDROID__
    // SDLActivity hands us a synthetic argv — discard it and synthesize
    // our own from assets/args.cfg after extracting bundled files into
    // internal storage. `version_tag` gates re-extraction; bump it when
    // bundled assets change (e.g. new keypad layout shipped).
    static std::vector<std::string> kAndroidArgs;
    static std::vector<const char*> kAndroidArgv;
    int n = j2me_android_bootstrap("v1", kAndroidArgs);
    if (n <= 0) {
        std::cerr << "[android] bootstrap failed\n";
        return 1;
    }
    kAndroidArgv.clear();
    for (auto& s : kAndroidArgs) kAndroidArgv.push_back(s.c_str());
    kAndroidArgv.push_back(nullptr);
    argc = n;
    argv = const_cast<char**>(kAndroidArgv.data());
#endif

    // Split args into positional (jar, class, WxH) and option flags. Keeps
    // the existing `j2me <jar> <class> [WxH]` invocation working, while
    // `--foo` options can appear anywhere after the required two positionals.
    std::vector<const char*> pos;
    bool     headless = false;
    bool     quiet    = false;
    uint64_t max_ticks = 0;
    uint64_t max_run_ms = 0;
    uint32_t tick_ms = 0;
    std::string ppm_path;
    std::string keys_arg;
    std::string bios_path;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << name << " requires an argument\n";
                std::exit(2);
            }
            return argv[++i];
        };
        if      (a == "--headless") headless = true;
        else if (a == "--quiet")    quiet = true;
        else if (a == "--ticks")    max_ticks  = (uint64_t)std::strtoull(need("--ticks"),  nullptr, 10);
        else if (a == "--run-ms")   max_run_ms = (uint64_t)std::strtoull(need("--run-ms"), nullptr, 10);
        else if (a == "--tick-ms")  tick_ms    = (uint32_t)std::strtoul (need("--tick-ms"), nullptr, 10);
        else if (a == "--ppm")      ppm_path   = need("--ppm");
        else if (a == "--keys")     keys_arg   = need("--keys");
        else if (a == "--bios")     bios_path  = need("--bios");
        else if (a == "--auto-res") { extern bool g_auto_res; g_auto_res = true; }
        else if (a == "--help" || a == "-h") { print_usage(argv[0]); return 0; }
        else if (!a.empty() && a[0] == '-') {
            std::cerr << "unknown option: " << a << "\n";
            print_usage(argv[0]);
            return 2;
        }
        else pos.push_back(argv[i]);
    }

    if (pos.size() < 2) {
        print_usage(argv[0]);
        return 1;
    }

    // Per-game config: ~/.j2me/<jar-stem>/config.json. Values here are
    // applied first; any CLI argument that follows overrides them. First
    // run writes a defaults file so users have something to edit.
    std::string jar_stem = fs::path(pos[0]).stem().string();
    GameConfig cfg = GameConfig::load(jar_stem);
    extern bool g_screen_explicit;
    if (auto wh = cfg.parsed_resolution()) {
        g_screen_w = wh->first;
        g_screen_h = wh->second;
        g_screen_explicit = true;
        if (!quiet)
            std::cerr << "[config] resolution from config.json: "
                      << g_screen_w << "x" << g_screen_h << "\n";
    }
    // Keypad prefs are deferred: Overlay::configure() picks them up when
    // Display::open() fires on the first paint.
    Display::instance().overlay().prefer_layout_by_name(cfg.keypad_layout);
    Display::instance().overlay().prefer_visible(cfg.keypad_visible);

    // Optional resolution override: "176x220" positional — wins over config.
    if (pos.size() >= 3) {
        int w = 0, h = 0;
        if (sscanf(pos[2], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            g_screen_w = w;
            g_screen_h = h;
            g_screen_explicit = true;
        }
    }

    // Configure the display before the MIDlet runs. open() is lazy (first
    // paint), so these settings are applied when it fires.
    if (headless) {
        Display::instance().set_headless(true);
        Display::instance().set_max_ticks(max_ticks);
        Display::instance().set_max_run_ms(max_run_ms);
        Display::instance().set_tick_ms(tick_ms);
        if (!keys_arg.empty())
            Display::instance().set_key_script(build_key_script(keys_arg));
    }

    auto dump_ppm = [&]() {
        if (ppm_path.empty()) return;
        if (Display::instance().save_ppm(ppm_path)) {
            if (!quiet)
                std::cerr << "[j2me] wrote " << ppm_path << "\n";
        } else {
            std::cerr << "[j2me] failed to write " << ppm_path << "\n";
        }
    };

    try {
        JarFile jar(pos[0]);
        std::unique_ptr<VM> vm_up;
        if (bios_path.empty()) {
            vm_up = std::make_unique<VM>(pos[0]);
        } else {
            if (!quiet) std::cout << "[bios] " << bios_path << "\n";
            vm_up = std::make_unique<VM>(pos[0], bios_path);
        }
        VM& vm = *vm_up;

        register_natives(vm, jar);
        register_graphics_natives(vm, jar);
        register_m3g_natives(vm, jar);
        register_lcdui_natives(vm);

        // Some titles (e.g. Jamdat framework games) draw via direct Graphics
        // calls and never invoke Canvas.repaint()/GameCanvas.flushGraphics().
        // Display::open() is normally lazy; force it in headless mode when a
        // PPM is requested so save_ppm has a surface to serialize even if the
        // game never goes through a paint path that opens it itself.
        if (headless && !ppm_path.empty())
            Display::instance().open(g_screen_w, g_screen_h);

        // Workaround for VServ ad SDK: if the JAR ships VservManager (used by
        // 365 Puzzle Club, 3D Bomberman, etc.), override its <clinit> to set
        // startMainApp=true. Without this, the MIDlet creates a VservManager,
        // which spins up an ad-fetch thread that hangs (no network, slow
        // timeouts). With startMainApp=true, the MIDlet skips ad fetch and
        // goes directly into the game.
        // VServ ad-SDK bypass: override the constructor so that creating a
        // VservManager just sets startMainApp=true and immediately calls
        // MIDlet.startMainApp() / startApp() to launch the game. Without this
        // the VServ thread tries to fetch ad data from the network and hangs.
        if (jar.has("VservManager.class")) {
            vm.register_native("VservManager", "<init>",
                "(Ljavax/microedition/midlet/MIDlet;Ljava/util/Hashtable;)V",
                [](VM& v2, Frame&, std::span<Slot> args) {
                    ObjRef midlet = args[1].as_ref();
                    // Only bypass the FIRST VServ instance (initial game
                    // launch). Mid-game ads re-create VservManager — those
                    // become no-ops (constructor doesn't reinitialize).
                    static bool bypassed_once = false;
                    if (bypassed_once) return;
                    bypassed_once = true;
                    v2.set_static("VservManager", "startMainApp", "Z",
                                  Slot::from_int(1));
                    HeapObject* mobj = v2.heap().deref(midlet);
                    if (!mobj || !mobj->klass) return;
                    // Run the BoxAL game's main-app constructor first (creates
                    // game canvas and other state), then call startMainApp.
                    auto invoke_if = [&](const char* name) {
                        MethodDef* m = mobj->klass->resolve_virtual(name, "()V");
                        if (!m) return;
                        try {
                            v2.invoke(m, mobj->klass, {Slot::from_ref(midlet)});
                        } catch (const QuitRequest&) { throw; }
                        catch (...) {}
                    };
                    invoke_if("constructorMainApp");
                    invoke_if("startMainApp");
                });
            vm.register_noop("VservManager", "showAtStart", "()V",
                "ad-network hook; we never display ads",
                [](VM&, Frame&, std::span<Slot>) {});
            vm.register_noop("VservManager", "showAtEnd", "()V",
                "ad-network hook; we never display ads",
                [](VM&, Frame&, std::span<Slot>) {});
            vm.register_noop("VservManager", "show", "()V",
                "ad-network hook; we never display ads",
                [](VM&, Frame&, std::span<Slot>) {});
            // 365 Puzzle Club's VservManager kicks off a Runnable background
            // thread from Canvas.showNotify() that dereferences the internal
            // Hashtable/Thread fields our <init> bypass never populated (it
            // replaces the constructor body whole). NPE at VservManager.a@13
            // is that thread pulling `notify-once` out of a null config map.
            // Turning run() into a no-op kills the ad-fetch Runnable body;
            // the `a(…)` overloads below cover every helper the background
            // code path might still reach through other entry points (paint,
            // keyPressed, Timer callbacks etc.).
            // register_native (not register_noop) because VservManager ships
            // bytecode for all of these — FillGap binding would be ignored.
            // register_native flips the NATIVE access flag on the existing
            // method so dispatch lands on our empty lambda.
            //
            // VservManager has a dozen+ obfuscated helpers named a/b/c/etc.
            // The ad-fetch thread (365 Puzzle) walks through several of them,
            // so rather than enumerate every signature we iterate the loaded
            // ClassDef and no-op every method whose name is a single letter
            // other than constructors and the public entrypoints we want to
            // keep reachable (showAtStart/show/showAtEnd handled separately).
            if (ClassDef* vsm = vm.loader().find("VservManager")) {
                for (MethodDef& md : vsm->methods) {
                    if (md.name == "<init>" || md.name == "<clinit>") continue;
                    if (md.name == "showAtStart" || md.name == "showAtEnd" ||
                        md.name == "show" || md.name == "paint" ||
                        md.name == "hideNotify" || md.name == "showNotify") continue;
                    if (md.name.size() != 1 && md.name != "run") continue;
                    // Push a zero/null of the right type so non-void helpers
                    // (e.g. VservManager.a(String)String) keep the caller's
                    // stack balanced.
                    auto ret = md.ret_type;
                    vm.register_native("VservManager", md.name, md.descriptor,
                        [ret](VM&, Frame& f, std::span<Slot>) {
                            switch (ret) {
                                case MethodDef::RetType::Int:    f.push_int(0);       break;
                                case MethodDef::RetType::Long:   f.push_long(0);      break;
                                case MethodDef::RetType::Float:  f.push_float(0.0f);  break;
                                case MethodDef::RetType::Double: f.push_double(0.0);  break;
                                case MethodDef::RetType::Ref:    f.push_ref(NULL_REF);break;
                                default: break;  // Void
                            }
                        });
                }
            }
        }

        if (!quiet) std::cout << "Starting MIDlet: " << pos[1] << "\n";
        vm.run(pos[1]);
        if (!quiet) std::cout << "MIDlet exited cleanly.\n";

    } catch (const QuitRequest&) {
        if (!quiet) std::cout << "User closed the window.\n";
    } catch (const JvmException& e) {
        std::cerr << "JVM exception: " << e.message;
        if (!e.location.empty()) std::cerr << " at " << e.location;
        std::cerr << "\n";
        dump_ppm();
        dump_stub_report();
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        dump_ppm();
        dump_stub_report();
        return 1;
    }

    dump_ppm();
    dump_stub_report();
    return 0;
}
