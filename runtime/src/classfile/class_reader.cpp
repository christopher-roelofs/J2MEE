#include "class_reader.hpp"
#include <vector>

uint8_t ClassReader::u1() {
    require(1);
    return m_data[m_pos++];
}

uint16_t ClassReader::u2() {
    require(2);
    uint16_t v = (static_cast<uint16_t>(m_data[m_pos]) << 8) |
                  static_cast<uint16_t>(m_data[m_pos + 1]);
    m_pos += 2;
    return v;
}

uint32_t ClassReader::u4() {
    require(4);
    uint32_t v = (static_cast<uint32_t>(m_data[m_pos    ]) << 24) |
                 (static_cast<uint32_t>(m_data[m_pos + 1]) << 16) |
                 (static_cast<uint32_t>(m_data[m_pos + 2]) <<  8) |
                  static_cast<uint32_t>(m_data[m_pos + 3]);
    m_pos += 4;
    return v;
}

uint64_t ClassReader::u8() {
    uint64_t hi = u4();
    uint64_t lo = u4();
    return (hi << 32) | lo;
}

std::vector<uint8_t> ClassReader::bytes(size_t n) {
    require(n);
    std::vector<uint8_t> out(m_data.begin() + m_pos,
                             m_data.begin() + m_pos + n);
    m_pos += n;
    return out;
}
