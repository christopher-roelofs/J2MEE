# Khronos M3G (JSR-184 Mobile 3D Graphics) reference implementation.
# Vendored from J2ME-Loader's app/src/main/cpp/m3g; Copyright Nokia, EPL-1.0.
# See LICENSE.EPL for license terms.
#
# Builds as a static library (m3g) that the main j2me runtime links against.
# Backend: GLES1.x (libGLESv1_CM) — M3G's reference uses fixed-function GLES.
# We do NOT include the Android-specific backend files (m3g_android.cpp,
# m3g_android_gl.cpp, CSynchronization.cpp, m3g_android_java_api.cpp); the
# desktop backend lives in src/midp/m3g_backend.cpp in the main runtime.

# M3G is structured as a unity build: m3g_core.c #includes every other
# m3g_*.c (each one starts with `#error included by m3g_core.c`).
add_library(m3g STATIC src/m3g_core.c)
target_include_directories(m3g PUBLIC inc src)

# Find GLES1 + EGL via pkg-config (Linux/Mesa). M3G's gl.h includes pull in
# GLES/gl.h and EGL/egl.h directly.
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLESV1_CM REQUIRED glesv1_cm)
pkg_check_modules(EGL REQUIRED egl)

target_include_directories(m3g PUBLIC ${GLESV1_CM_INCLUDE_DIRS} ${EGL_INCLUDE_DIRS})
target_link_libraries(m3g PUBLIC ${GLESV1_CM_LIBRARIES} ${EGL_LIBRARIES})

# Treat vendored sources as system headers — they trip our -Wpedantic etc.
target_compile_options(m3g PRIVATE
    -w                       # silence vendor warnings completely
    -Wno-error
    # M3G expects these to be set by the build system
    -DM3G_TARGET_GENERIC      # not Android, not Symbian
    -DM3G_GL_ES_1_1           # GL ES 1.1 backend
)
