#pragma once

// Thin wrapper around vendored Sonivox EAS (third_party/sonivox).
// Handles OTA (Nokia Smart Messaging), SMF/SP-MIDI, XMF, iMelody, RTTTL.
// Intentionally minimal — not yet wired into JSR-135 Player natives.

#include <cstdint>
#include <cstddef>
#include <vector>

namespace eas {

// Initialise the library (one-shot; safe to call multiple times).
// Returns false if the EAS engine failed to start.
bool init();

// Shut down the engine and release all streams.
void shutdown();

// Holds a single decoded/streaming sound. Owns an EAS handle + a copy of
// the source bytes (EAS reads from a memory buffer we keep alive).
class Stream {
public:
    Stream() = default;
    ~Stream();

    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    Stream(Stream&&) noexcept;
    Stream& operator=(Stream&&) noexcept;

    // Open a blob of any EAS-supported format. Returns false on parse error.
    bool open(const uint8_t* data, size_t size);

    // Render up to `frame_count` stereo 16-bit frames into `out`.
    // Returns number of frames actually produced; 0 means EOS (unless looping).
    int32_t render(int16_t* out, int32_t frame_count);

    // 0..100. Default 100.
    void set_volume(int v);

    bool is_open() const { return m_handle != nullptr; }

private:
    void close();

    void* m_handle = nullptr;        // EAS_HANDLE (opaque)
    std::vector<uint8_t> m_bytes;    // keep data alive for EAS
    // EAS_FILE + its host wrapper live here if we end up needing them
    void* m_file = nullptr;
};

// Engine-wide sample rate & channel count (compile-time from Sonivox config).
int sample_rate();
int channels();

} // namespace eas
