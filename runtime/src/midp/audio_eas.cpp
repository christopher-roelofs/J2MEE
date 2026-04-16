// Thin C++ wrapper around vendored Sonivox EAS (third_party/sonivox).
// Not yet wired into JSR-135 Player natives — this compilation unit exists
// to verify the vendor build, expose a minimal API, and give a hook point
// for future Player integration.

#include "audio_eas.hpp"

extern "C" {
#include "libsonivox/eas.h"
#include "libsonivox/eas_types.h"
}

#include <cstdio>
#include <cstring>
#include <utility>

namespace eas {

namespace {

// One EAS instance shared across all Stream objects.
EAS_DATA_HANDLE g_eas = nullptr;
const S_EAS_LIB_CONFIG* g_cfg = nullptr;

// Memory-backed file handle — lives alongside a Stream::m_bytes buffer.
struct MemFile {
    const uint8_t* data;
    int32_t size;
};

int mem_read_at(void* handle, void* buf, int offset, int size) {
    auto* f = static_cast<MemFile*>(handle);
    if (offset >= f->size) return 0;
    int avail = f->size - offset;
    if (size > avail) size = avail;
    std::memcpy(buf, f->data + offset, size);
    return size;
}

int mem_size(void* handle) {
    return static_cast<MemFile*>(handle)->size;
}

} // namespace

bool init() {
    if (g_eas) return true;
    if (EAS_Init(&g_eas) != EAS_SUCCESS) {
        fprintf(stderr, "[eas] EAS_Init failed\n");
        g_eas = nullptr;
        return false;
    }
    g_cfg = EAS_Config();
    return true;
}

void shutdown() {
    if (!g_eas) return;
    EAS_Shutdown(g_eas);
    g_eas = nullptr;
    g_cfg = nullptr;
}

int sample_rate() { return g_cfg ? g_cfg->sampleRate : 22050; }
int channels()    { return g_cfg ? g_cfg->numChannels : 2; }

// ─── Stream ──────────────────────────────────────────────────────────────────

Stream::~Stream() { close(); }

Stream::Stream(Stream&& o) noexcept
    : m_handle(o.m_handle), m_bytes(std::move(o.m_bytes)), m_file(o.m_file) {
    o.m_handle = nullptr;
    o.m_file = nullptr;
}

Stream& Stream::operator=(Stream&& o) noexcept {
    if (this != &o) {
        close();
        m_handle = o.m_handle;
        m_bytes = std::move(o.m_bytes);
        m_file = o.m_file;
        o.m_handle = nullptr;
        o.m_file = nullptr;
    }
    return *this;
}

bool Stream::open(const uint8_t* data, size_t size) {
    if (!g_eas && !init()) return false;
    close();

    m_bytes.assign(data, data + size);

    auto* mf = new MemFile{m_bytes.data(), static_cast<int32_t>(m_bytes.size())};
    auto* fl = new EAS_FILE;
    fl->handle  = mf;
    fl->readAt  = mem_read_at;
    fl->size    = mem_size;
    m_file = fl;

    EAS_HANDLE h = nullptr;
    if (EAS_OpenFile(g_eas, fl, &h) != EAS_SUCCESS) {
        fprintf(stderr, "[eas] EAS_OpenFile failed\n");
        delete mf;
        delete fl;
        m_file = nullptr;
        return false;
    }
    if (EAS_Prepare(g_eas, h) != EAS_SUCCESS) {
        fprintf(stderr, "[eas] EAS_Prepare failed\n");
        EAS_CloseFile(g_eas, h);
        delete mf;
        delete fl;
        m_file = nullptr;
        return false;
    }
    m_handle = h;
    return true;
}

int32_t Stream::render(int16_t* out, int32_t frame_count) {
    if (!m_handle || !g_eas || !g_cfg) return 0;
    // EAS renders in fixed-size chunks of `mixBufferSize` frames.
    int32_t done = 0;
    const int32_t chunk = g_cfg->mixBufferSize;
    while (done < frame_count) {
        int32_t want = chunk;
        if (done + want > frame_count) break; // EAS won't render partial
        EAS_I32 generated = 0;
        EAS_RESULT r = EAS_Render(g_eas,
                                  out + done * g_cfg->numChannels,
                                  want, &generated);
        if (r != EAS_SUCCESS || generated <= 0) break;
        done += generated;
    }
    return done;
}

void Stream::set_volume(int v) {
    if (!m_handle || !g_eas) return;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    EAS_SetVolume(g_eas, static_cast<EAS_HANDLE>(m_handle), v);
}

void Stream::close() {
    if (m_handle && g_eas) {
        EAS_CloseFile(g_eas, static_cast<EAS_HANDLE>(m_handle));
    }
    m_handle = nullptr;
    if (m_file) {
        auto* fl = static_cast<EAS_FILE*>(m_file);
        delete static_cast<MemFile*>(fl->handle);
        delete fl;
        m_file = nullptr;
    }
    m_bytes.clear();
}

} // namespace eas
