// Android startup shims for the J2ME runtime.
//
// The existing runtime assumes a Linux filesystem: std::ifstream on raw
// paths, $HOME/.j2me/… for RMS and per-game config, /usr/share/fonts/…
// for TTF files, ../assets/keypad/… for the overlay. On Android, read-only
// data lives inside the APK behind AAssetManager and only read-write data
// goes to internal storage.
//
// Rather than rewrite every file-open, we extract the bundled APK assets
// once (first launch or when the APK's install-time version bumps) to
// internal storage, then set HOME to that directory. After that, the
// existing path code — game_config.cpp, natives.cpp, overlay.cpp — all
// resolve correctly relative to HOME with minor tweaks in those files.
//
// Layout after extraction (HOME = SDL_AndroidGetInternalStoragePath()):
//   $HOME/assets/keypad/default/arrow-up.png   …
//   $HOME/assets/keypad/layouts/minimal/layout.json
//   $HOME/fonts/DejaVuSans.ttf                 …
//   $HOME/fonts/fa-solid-900.ttf
//   $HOME/games/<jar-name>.jar
//   $HOME/.j2me/<jar-stem>/…                   (RMS + config, writable)
//   $HOME/.stamp.<version>                     (extraction marker)

#ifdef __ANDROID__

#include <SDL.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <jni.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <pthread.h>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "j2me", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "j2me", __VA_ARGS__)

namespace {

// Pump: copy everything written to `fd_in` into logcat tagged `tag`.
// Runs on its own detached pthread so the runtime's std::cerr / stderr
// / perror() calls appear alongside our own [j2me] messages instead of
// disappearing into the void (SDL 2.30+ dropped its auto-redirect to
// /files/stderr.txt).
struct PumpArgs { int fd; const char* tag; };
void* log_pump(void* raw) {
    auto* args = static_cast<PumpArgs*>(raw);
    char buf[1024];
    std::string line;
    for (;;) {
        ssize_t n = read(args->fd, buf, sizeof(buf));
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                __android_log_write(ANDROID_LOG_INFO, args->tag, line.c_str());
                line.clear();
            } else if (buf[i] != '\r') {
                line.push_back(buf[i]);
            }
        }
    }
    if (!line.empty())
        __android_log_write(ANDROID_LOG_INFO, args->tag, line.c_str());
    return nullptr;
}

void redirect_fd_to_logcat(int target_fd, const char* tag) {
    int p[2];
    if (pipe(p) != 0) return;
    dup2(p[1], target_fd);
    close(p[1]);
    auto* args = new PumpArgs{p[0], tag};
    pthread_t t;
    if (pthread_create(&t, nullptr, log_pump, args) == 0)
        pthread_detach(t);
}

// Read the manifest — one relative path per line — and copy each
// listed file out of AAssetManager into home_dir/<path>.
bool extract_assets(AAssetManager* am, const fs::path& home) {
    AAsset* mf = AAssetManager_open(am, "manifest.txt", AASSET_MODE_BUFFER);
    if (!mf) { LOGE("manifest.txt missing from APK assets"); return false; }
    const char* buf = (const char*)AAsset_getBuffer(mf);
    size_t len = (size_t)AAsset_getLength(mf);
    std::string text(buf, len);
    AAsset_close(mf);

    std::istringstream iss(text);
    std::string rel;
    int copied = 0;
    while (std::getline(iss, rel)) {
        while (!rel.empty() && (rel.back() == '\r' || rel.back() == ' '))
            rel.pop_back();
        if (rel.empty() || rel[0] == '#') continue;

        AAsset* a = AAssetManager_open(am, rel.c_str(), AASSET_MODE_STREAMING);
        if (!a) { LOGE("asset missing: %s", rel.c_str()); continue; }
        fs::path dst = home / rel;
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        FILE* f = std::fopen(dst.c_str(), "wb");
        if (!f) { LOGE("open for write failed: %s", dst.c_str()); AAsset_close(a); continue; }
        char chunk[8192];
        int n;
        while ((n = AAsset_read(a, chunk, sizeof(chunk))) > 0)
            std::fwrite(chunk, 1, (size_t)n, f);
        std::fclose(f);
        AAsset_close(a);
        ++copied;
    }
    LOGI("extracted %d assets into %s", copied, home.c_str());
    return true;
}

// Grab AAssetManager via JNI — SDL hands us the activity + env.
AAssetManager* get_asset_manager() {
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();
    if (!env || !activity) return nullptr;
    jclass cls = env->GetObjectClass(activity);
    jmethodID getAssets = env->GetMethodID(cls, "getAssets",
                                           "()Landroid/content/res/AssetManager;");
    jobject assetsJ = env->CallObjectMethod(activity, getAssets);
    AAssetManager* am = AAssetManager_fromJava(env, assetsJ);
    // Leak local refs intentionally; activity owns them for process lifetime.
    return am;
}

} // namespace

// Called from main.cpp under __ANDROID__ before argv is consumed.
// Writes its synthesized argv into `out_argv`, returns argc.
// Owned storage lives in static vectors — fine for process lifetime.
extern "C" int j2me_android_bootstrap(const char* version_tag,
                                      std::vector<std::string>& out_argv) {
    // Route stdout + stderr into logcat so std::cerr / fprintf(stderr, …)
    // calls from the runtime show up under `adb logcat`. Line-buffer both
    // so short messages don't get batched indefinitely.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    redirect_fd_to_logcat(STDOUT_FILENO, "j2me.stdout");
    redirect_fd_to_logcat(STDERR_FILENO, "j2me.stderr");

    const char* home_c = SDL_AndroidGetInternalStoragePath();
    if (!home_c) { LOGE("SDL_AndroidGetInternalStoragePath == null"); return 0; }
    fs::path home = home_c;
    std::error_code ec;
    fs::create_directories(home, ec);

    // Extract-once gate. Bump version_tag on any asset change to force
    // re-extraction (Gradle can feed android:versionName through).
    fs::path stamp = home / (std::string(".stamp.") + version_tag);
    if (!fs::exists(stamp)) {
        AAssetManager* am = get_asset_manager();
        if (!am) { LOGE("no AAssetManager"); return 0; }
        // Clear any previous stamps so we don't accumulate cruft.
        for (auto& e : fs::directory_iterator(home, ec))
            if (e.path().filename().string().rfind(".stamp.", 0) == 0)
                fs::remove(e.path(), ec);
        if (!extract_assets(am, home)) return 0;
        std::ofstream(stamp).put('1');
    } else {
        LOGI("assets already extracted for %s", version_tag);
    }

    // HOME is what game_config.cpp and natives.cpp branch on. We point it
    // at internal storage so ~/.j2me/<game>/ resolves there.
    setenv("HOME", home.c_str(), 1);

    // Font + keypad paths are rooted against these env vars — read by
    // graphics_natives.cpp and overlay.cpp on Android. J2ME_ASSET_DIR
    // points at HOME itself because the manifest's keypad/... entries
    // extract to $HOME/keypad/..., not $HOME/assets/keypad/...
    setenv("J2ME_FONT_DIR",  (home / "fonts").c_str(), 1);
    setenv("J2ME_ASSET_DIR", home.c_str(), 1);

    // SDL_mixer's timidity backend (music_timidity.c TIMIDITY_Open) checks
    // TIMIDITY_CFG first via SDL_getenv — then falls back to absolute paths
    // like /etc/timidity/freepats.cfg which don't exist on Android. Point
    // it at the cfg we ship in APK assets; SDL_RWFromFile routes the
    // relative path through AAssetManager so both the cfg and the TimGM6mb
    // .sf2 it references resolve inside the APK.
    setenv("TIMIDITY_CFG", "timidity.cfg", 1);

    // SDL 2.30 defaults to OpenSLES on Android; on Pixel 7 / Android 15 the
    // OpenSLES AudioTrack path stops with 0 frames delivered. AAudio is
    // recommended for API 26+ and works reliably. Pick it explicitly via
    // SDL_HINT_AUDIODRIVER (consumed during SDL_InitAudio).
    setenv("SDL_AUDIODRIVER", "aaudio", 1);

    // args.cfg — plain text, one field per line:
    //   line 1: jar filename inside $HOME/games/
    //   line 2: MIDlet class (dotted)
    //   line 3: optional WxH  (blank line = auto)
    //   lines 4+ : extra raw arguments, one per line (e.g. --quiet, --bios)
    out_argv.clear();
    out_argv.push_back("j2me");

    fs::path args_path = home / "args.cfg";
    std::ifstream cfg(args_path);
    if (!cfg) {
        LOGE("args.cfg not found at %s", args_path.c_str());
        return 0;
    }
    std::string jar_name, midlet, res;
    std::getline(cfg, jar_name);
    std::getline(cfg, midlet);
    std::getline(cfg, res);

    // Resolve jar path against $HOME/games/. If an absolute path is given
    // (user dropped a full path in args.cfg), honour it.
    fs::path jar_path = fs::path(jar_name).is_absolute()
        ? fs::path(jar_name)
        : home / "games" / jar_name;
    out_argv.push_back(jar_path.string());
    out_argv.push_back(midlet);
    if (!res.empty()) out_argv.push_back(res);

    std::string extra;
    while (std::getline(cfg, extra)) {
        while (!extra.empty() && (extra.back() == '\r' || extra.back() == ' '))
            extra.pop_back();
        if (!extra.empty()) out_argv.push_back(extra);
    }

    LOGI("argv:");
    for (auto& s : out_argv) LOGI("  %s", s.c_str());
    return (int)out_argv.size();
}

#endif  // __ANDROID__
