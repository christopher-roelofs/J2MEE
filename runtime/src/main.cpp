#include <iostream>
#include <cstdlib>
#include "util/jar.hpp"
#include "vm/vm.hpp"
#include "midp/natives.hpp"

// Declared in interpreter.cpp — set true to trace every opcode
extern bool g_trace;

// Defined in graphics_natives.cpp — override before register_graphics_natives
extern int g_screen_w, g_screen_h;

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::cerr << "usage: j2me <file.jar> <MIDletClass> [WxH]\n";
        return 1;
    }

    g_trace = (std::getenv("J2ME_TRACE") != nullptr);

    // Optional resolution override: "176x220"
    if (argc >= 4) {
        int w = 0, h = 0;
        if (sscanf(argv[3], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
            g_screen_w = w;
            g_screen_h = h;
        }
    }

    try {
        JarFile jar(argv[1]);
        VM vm(argv[1]);

        register_natives(vm, jar);
        register_graphics_natives(vm, jar);

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
            vm.register_native("VservManager", "showAtStart", "()V",
                [](VM&, Frame&, std::span<Slot>) {});
            vm.register_native("VservManager", "showAtEnd", "()V",
                [](VM&, Frame&, std::span<Slot>) {});
        }

        std::cout << "Starting MIDlet: " << argv[2] << "\n";
        vm.run(argv[2]);
        std::cout << "MIDlet exited cleanly.\n";

    } catch (const QuitRequest&) {
        std::cout << "User closed the window.\n";
    } catch (const JvmException& e) {
        std::cerr << "JVM exception: " << e.message;
        if (!e.location.empty()) std::cerr << " at " << e.location;
        std::cerr << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
