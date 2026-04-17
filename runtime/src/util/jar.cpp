#include "jar.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>
#include <zlib.h>

// ─── ZIP format constants ─────────────────────────────────────────────────────

static constexpr uint32_t SIG_LOCAL   = 0x04034b50;
static constexpr uint32_t SIG_CENTRAL = 0x02014b50;
static constexpr uint32_t SIG_EOCD    = 0x06054b50;

static constexpr uint16_t METHOD_STORED   = 0;
static constexpr uint16_t METHOD_DEFLATED = 8;

// ─── Little-endian helpers ────────────────────────────────────────────────────

static uint16_t read_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
static uint32_t read_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])        |
           (static_cast<uint32_t>(p[1]) <<  8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// ─── Inflate via zlib ─────────────────────────────────────────────────────────

std::vector<uint8_t> JarFile::inflate(const uint8_t* src, size_t src_len,
                                      uint32_t uncompressed_size) {
    std::vector<uint8_t> out(uncompressed_size);

    z_stream zs{};
    zs.next_in   = const_cast<Bytef*>(src);
    zs.avail_in  = static_cast<uInt>(src_len);
    zs.next_out  = out.data();
    zs.avail_out = static_cast<uInt>(uncompressed_size);

    // -MAX_WBITS: raw deflate (no zlib header), as used inside ZIP
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
        throw std::runtime_error("zlib inflateInit2 failed");

    int ret = ::inflate(&zs, Z_FINISH);
    inflateEnd(&zs);

    if (ret != Z_STREAM_END)
        throw std::runtime_error("zlib inflate failed: " + std::to_string(ret));

    return out;
}

// ─── ZIP loading ──────────────────────────────────────────────────────────────

void JarFile::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw std::runtime_error("Cannot open JAR: " + path);

    size_t file_size = static_cast<size_t>(f.tellg());
    f.seekg(0);

    std::vector<uint8_t> data(file_size);
    if (!f.read(reinterpret_cast<char*>(data.data()), file_size))
        throw std::runtime_error("Failed to read JAR: " + path);

    const uint8_t* buf = data.data();

    // ── Find End of Central Directory (EOCD) ─────────────────────────────────
    // Scan backward for signature 0x06054b50.
    // EOCD can have a variable-length comment at the end, but JAR files
    // never embed comments, so we just scan the last ~64 KB.
    constexpr size_t EOCD_SIZE = 22;
    if (file_size < EOCD_SIZE)
        throw std::runtime_error("File too small to be a JAR");

    size_t eocd_offset = SIZE_MAX;
    size_t scan_start = file_size >= 65536 + EOCD_SIZE
                        ? file_size - 65536 - EOCD_SIZE : 0;
    for (size_t i = file_size - EOCD_SIZE; i >= scan_start; --i) {
        if (read_u32(buf + i) == SIG_EOCD) {
            eocd_offset = i;
            break;
        }
        if (i == 0) break;
    }
    if (eocd_offset == SIZE_MAX)
        throw std::runtime_error("No EOCD signature found in: " + path);

    const uint8_t* eocd    = buf + eocd_offset;
    uint16_t entry_count   = read_u16(eocd + 10);
    uint32_t cd_size       = read_u32(eocd + 12);
    uint32_t cd_offset     = read_u32(eocd + 16);

    // ── Walk Central Directory ────────────────────────────────────────────────
    const uint8_t* cd = buf + cd_offset;
    const uint8_t* cd_end = cd + cd_size;

    for (uint16_t i = 0; i < entry_count; ++i) {
        if (cd + 46 > cd_end || read_u32(cd) != SIG_CENTRAL)
            throw std::runtime_error("Corrupt central directory in: " + path);

        uint16_t method        = read_u16(cd + 10);
        uint32_t comp_size     = read_u32(cd + 20);
        uint32_t uncomp_size   = read_u32(cd + 24);
        uint16_t name_len      = read_u16(cd + 28);
        uint16_t extra_len     = read_u16(cd + 30);
        uint16_t comment_len   = read_u16(cd + 32);
        uint32_t local_offset  = read_u32(cd + 42);

        std::string name(reinterpret_cast<const char*>(cd + 46), name_len);
        cd += 46 + name_len + extra_len + comment_len;

        // Skip directory entries
        if (!name.empty() && name.back() == '/')
            continue;

        // ── Read local file header ────────────────────────────────────────────
        const uint8_t* local = buf + local_offset;
        if (local + 30 > buf + file_size || read_u32(local) != SIG_LOCAL)
            throw std::runtime_error("Corrupt local header for: " + name);

        uint16_t local_name_len  = read_u16(local + 26);
        uint16_t local_extra_len = read_u16(local + 28);
        const uint8_t* entry_data = local + 30 + local_name_len + local_extra_len;

        std::vector<uint8_t> content;
        if (method == METHOD_STORED) {
            content.assign(entry_data, entry_data + uncomp_size);
        } else if (method == METHOD_DEFLATED) {
            content = inflate(entry_data, comp_size, uncomp_size);
        } else {
            throw std::runtime_error("Unsupported ZIP method " +
                                     std::to_string(method) + " for: " + name);
        }

        m_entries.emplace(std::move(name), std::move(content));
    }
}

// ─── Public API ───────────────────────────────────────────────────────────────

JarFile::JarFile(const std::string& path) : m_path(path) {
    load(path);
}

bool JarFile::has(const std::string& name) const {
    return m_entries.count(name) > 0;
}

const std::vector<uint8_t>& JarFile::get(const std::string& name) const {
    auto it = m_entries.find(name);
    if (it == m_entries.end())
        throw std::runtime_error("JAR entry not found: " + name);
    return it->second;
}

std::vector<std::string> JarFile::entries() const {
    std::vector<std::string> names;
    names.reserve(m_entries.size());
    for (auto& [k, _] : m_entries) names.push_back(k);
    return names;
}

std::vector<std::string> JarFile::entries_with_suffix(const std::string& suffix) const {
    std::vector<std::string> result;
    for (auto& [name, _] : m_entries)
        if (name.size() >= suffix.size() &&
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0)
            result.push_back(name);
    return result;
}

std::string JarFile::resolve(const std::string& name) const {
    if (name.empty()) return "";
    if (m_entries.count(name)) return name;
    if (name[0] == '/') {
        std::string stripped = name.substr(1);
        if (m_entries.count(stripped)) return stripped;
        return resolve(stripped);
    }
    // Locale prefix strip ("en/foo.str" → "foo.str").
    auto slash = name.find('/');
    if (slash != std::string::npos) {
        std::string tail = name.substr(slash + 1);
        if (m_entries.count(tail)) return tail;
    }
    // No extension in the basename → try common image extensions. Fixes
    // titles like Super Puzzle Bobble that call Image.createImage("ballsas")
    // where the real entry is "ballsas.png".
    auto last_slash = name.find_last_of('/');
    auto basename_start = (last_slash == std::string::npos) ? 0 : last_slash + 1;
    if (name.find('.', basename_start) == std::string::npos) {
        static const char* kExts[] = { ".png", ".jpg", ".jpeg", ".gif" };
        for (const char* ext : kExts) {
            std::string with_ext = name + ext;
            if (m_entries.count(with_ext)) return with_ext;
        }
    }
    return "";
}
