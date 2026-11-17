// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace dc::utf8 {

inline bool continuation(unsigned char ch) {
    return (ch & 0xc0) == 0x80;
}

inline void backspace(std::string &text) {
    if (!text.empty()) {
        while (continuation(text.back())) {
            text.pop_back();
        }
        text.pop_back();
    }
}

inline void append_latin1(std::string &text, unsigned char ch) {
    if (ch >= 0x80) {
        text.push_back(char(0xc0 | (ch >> 6)));
        ch = 0x80 | (ch & 0x3f);
    }
    text.push_back(char(ch));
}

// Return the longest well-formed printable prefix within both bounds.
inline size_t prefix(std::string_view text, size_t characters, size_t bytes) {
    size_t offset = 0;
    const size_t end = text.size() < bytes ? text.size() : bytes;
    for (size_t count = 0; offset < end && count < characters; ++count) {
        const auto head = static_cast<unsigned char>(text[offset]);
        const unsigned length = head < 0x80 ? 1 : head >= 0xc2 && head <= 0xdf ? 2 :
                                head >= 0xe0 && head <= 0xef ? 3 :
                                head >= 0xf0 && head <= 0xf4 ? 4 : 0;
        if (!length || length > end - offset) {
            break;
        }
        uint32_t codepoint = length == 1 ? head : head & (0x7f >> length);
        for (unsigned i = 1; i < length; ++i) {
            const auto next = static_cast<unsigned char>(text[offset + i]);
            if (!continuation(next)) {
                return offset;
            }
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        constexpr uint32_t minimum[] = {0, 0x20, 0x80, 0x800, 0x10000};
        if (codepoint < minimum[length] || (codepoint >= 0x7f && codepoint <= 0x9f) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) {
            break;
        }
        offset += length;
    }
    return offset;
}

} // namespace dc::utf8
