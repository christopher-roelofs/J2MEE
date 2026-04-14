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

        std::cout << "Starting MIDlet: " << argv[2] << "\n";
        vm.run(argv[2]);
        std::cout << "MIDlet exited cleanly.\n";

    } catch (const QuitRequest&) {
        std::cout << "User closed the window.\n";
    } catch (const JvmException& e) {
        std::cerr << "JVM exception: " << e.message << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
