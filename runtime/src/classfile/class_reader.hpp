#pragma once
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Big-endian binary reader over a byte span.
// JVM class files are always big-endian.
class ClassReader {
public:
    explicit ClassReader(std::span<const uint8_t> data) : m_data(data), m_pos(0) {}

    uint8_t  u1();
    uint16_t u2();
    uint32_t u4();
    uint64_t u8();
    int8_t   s1() { return static_cast<int8_t>(u1()); }
    int16_t  s2() { return static_cast<int16_t>(u2()); }
    int32_t  s4() { return static_cast<int32_t>(u4()); }

    // Read exactly n bytes into a new vector
    std::vector<uint8_t> bytes(size_t n);

    size_t pos() const { return m_pos; }
    size_t remaining() const { return m_data.size() - m_pos; }

private:
    std::span<const uint8_t> m_data;
    size_t m_pos;

    void require(size_t n) const {
        if (m_pos + n > m_data.size())
            throw std::runtime_error("Unexpected end of class file at offset " +
                                     std::to_string(m_pos));
    }
};
