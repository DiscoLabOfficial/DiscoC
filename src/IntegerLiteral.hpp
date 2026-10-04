#pragma once
#include <string>
#include <cstdint>

namespace DiscoNumeric {
// std::stoll base 0 does not recognize the language's binary prefix.
inline std::int64_t parse(const std::string& text, std::size_t* consumed = nullptr, int base = 0) {
    const auto sign = !text.empty() && (text.front() == '-' || text.front() == '+') ? 1u : 0u;
    if (base == 0 && text.size() >= sign + 2 && text[sign] == '0' && (text[sign + 1] == 'b' || text[sign + 1] == 'B')) {
        const auto digits = text.substr(0, sign) + text.substr(sign + 2);
        std::size_t count = 0;
        const auto value = std::stoll(digits, &count, 2);
        if (consumed) *consumed = count + 2;
        return value;
    }
    return std::stoll(text, consumed, base);
}
}
