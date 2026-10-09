#pragma once

#include <algorithm>
#include <cstddef>

// Proven lower bound: SP >= the linker's common stack floor + credit.
// No check is moved earlier across a store, call, or other observable effect.
class GSUStackCheckCredit {
public:
    void reset() { m_bytes = 0; }
    bool covers(std::size_t required) const { return required <= m_bytes; }
    void checked(std::size_t required) { m_bytes = std::max(m_bytes, std::min(required, std::size_t{65535})); }
    void decrease(std::size_t bytes) { m_bytes = bytes > m_bytes ? 0 : m_bytes - bytes; }
    void increase(std::size_t bytes) { m_bytes += std::min(bytes, std::size_t{65535} - m_bytes); }
private:
    std::size_t m_bytes = 0;
};
