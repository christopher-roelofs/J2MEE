#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Minimal JAR (ZIP) reader.
// Reads all entries into memory on open; JAR files for CLDC games are small.
class JarFile {
public:
    explicit JarFile(const std::string& path);

    // Returns true if an entry with this name exists.
    bool has(const std::string& name) const;

    // Returns the raw (decompressed) bytes for an entry.
    // Throws std::runtime_error if the entry does not exist.
    const std::vector<uint8_t>& get(const std::string& name) const;

    // Resolve a resource name to an existing entry, trying common fallbacks:
    //   - exact match
    //   - strip a leading "/"
    //   - strip a leading locale-style prefix ("en/foo" → "foo")
    //   - if no extension present, try common image extensions
    //     (.png, .jpg, .jpeg, .gif)
    // Returns the matched entry name, or empty string if nothing matched.
    // Several titles (Super Puzzle Bobble, some Jamdat games) call
    // Image.createImage("ballsas") where the real entry is "ballsas.png".
    std::string resolve(const std::string& name) const;

    // All entry names in the archive.
    std::vector<std::string> entries() const;

    // All entry names matching a suffix (e.g. ".class").
    std::vector<std::string> entries_with_suffix(const std::string& suffix) const;

    const std::string& path() const { return m_path; }

private:
    std::string m_path;
    std::unordered_map<std::string, std::vector<uint8_t>> m_entries;

    void load(const std::string& path);
    static std::vector<uint8_t> inflate(const uint8_t* src, size_t src_len,
                                        uint32_t uncompressed_size);
};
