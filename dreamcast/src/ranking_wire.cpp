// SPDX-License-Identifier: GPL-2.0-or-later
#include "ranking_wire.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>

namespace dc::ranking {
namespace {

constexpr char hex_digits[] = "0123456789abcdef";

void hex_byte(std::string &out, uint8_t byte) {
    out += hex_digits[byte >> 4];
    out += hex_digits[byte & 15];
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_alphanumeric(char c) {
    return is_digit(c) || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

class Sha1 {
public:
    void add(const uint8_t *bytes, size_t count) {
        total_ += count;
        while (count != 0) {
            const size_t part = std::min(count, block_.size() - used_);
            std::copy_n(bytes, part, block_.begin() + used_);
            bytes += part;
            count -= part;
            used_ += part;
            if (used_ == block_.size()) {
                compress();
                used_ = 0;
            }
        }
    }

    std::string finish() {
        const uint64_t bits = total_ * 8;
        const uint8_t padding[64] = {0x80};
        add(padding, used_ < 56 ? 56 - used_ : 120 - used_);
        uint8_t length[8];
        for (unsigned i = 0; i < 8; ++i) {
            length[i] = bits >> (8 * (7 - i));
        }
        add(length, sizeof(length));
        std::string digest;
        digest.reserve(40);
        for (uint32_t word : state_) {
            for (int shift = 24; shift >= 0; shift -= 8) {
                hex_byte(digest, word >> shift);
            }
        }
        return digest;
    }

private:
    static uint32_t rotate(uint32_t word, unsigned count) {
        return (word << count) | (word >> (32 - count));
    }

    void compress() {
        uint32_t words[80];
        for (unsigned i = 0; i < 16; ++i) {
            const uint8_t *bytes = block_.data() + i * 4;
            words[i] = (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) |
                       (uint32_t(bytes[2]) << 8) | uint32_t(bytes[3]);
        }
        for (unsigned i = 16; i < 80; ++i) {
            words[i] = rotate(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
        }
        auto [a, b, c, d, e] = state_;
        for (unsigned i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5a827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdc;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6;
            }
            const uint32_t next = rotate(a, 5) + f + e + k + words[i];
            e = d;
            d = c;
            c = rotate(b, 30);
            b = a;
            a = next;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
    }

    std::array<uint32_t, 5> state_ = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    std::array<uint8_t, 64> block_{};
    size_t used_ = 0;
    uint64_t total_ = 0;
};

std::string classic_name(const std::string &utf8) {
    // The Linux song.ini uses Latin-1 names. The wire hashes the same bytes it sends.
    std::string name;
    for (size_t i = 0; i < utf8.size(); ++i) {
        const auto lead = uint8_t(utf8[i]);
        if (lead < 0x80) {
            name += utf8[i];
        } else if ((lead == 0xc2 || lead == 0xc3) && i + 1 < utf8.size() &&
                   (uint8_t(utf8[i + 1]) & 0xc0) == 0x80) {
            name += char(((lead & 3) << 6) | (uint8_t(utf8[++i]) & 63));
        } else {
            // Keep the name intact when it cannot be represented as Latin-1.
            return utf8;
        }
    }
    return name;
}

void cereal_string(std::string &out, std::string_view data) {
    out += "s" + std::to_string(data.size()) + "\n";
    out += data;
}

std::string cereal_scores(const Tables &tables) {
    size_t table_count = 0;
    size_t tuple_count = 0;
    for (const auto &table : tables) {
        table_count += !table.empty();
        tuple_count += table.size();
    }

    // Cerealizer emits the dictionary, then its lists, then the immutable tuples.
    std::string payload = "cereal1\n" + std::to_string(1 + table_count + tuple_count) + "\ndict\n";
    for (size_t i = 0; i < table_count; ++i) {
        payload += "list\n";
    }
    for (size_t difficulty = 0; difficulty < tables.size(); ++difficulty) {
        for (const auto &score : tables[difficulty]) {
            const auto points = std::to_string(score.points);
            const auto stars = std::to_string(score.stars);
            const auto name = classic_name(score.name);
            const auto fields = std::to_string(difficulty) + points + stars + name;
            Sha1 hash;
            hash.add(reinterpret_cast<const uint8_t *>(fields.data()), fields.size());
            // A Python 2 int on Linux i686 promotes to long past INT32_MAX.
            const char *type = score.points > INT32_MAX ? "l" : "i";
            payload += std::string("tuple\n4\n") + type + points + "\ni" + stars + "\n";
            cereal_string(payload, name);
            cereal_string(payload, hash.finish());
        }
    }
    payload += std::to_string(table_count) + "\n";
    size_t list_index = 1;
    for (size_t difficulty = 0; difficulty < tables.size(); ++difficulty) {
        if (!tables[difficulty].empty()) {
            payload += "r" + std::to_string(list_index++) + "\ni" + std::to_string(difficulty) + "\n";
        }
    }
    size_t tuple_index = table_count + 1;
    for (const auto &table : tables) {
        if (table.empty()) {
            continue;
        }
        payload += std::to_string(table.size()) + "\n";
        for (size_t i = 0; i < table.size(); ++i) {
            payload += "r" + std::to_string(tuple_index++) + "\n";
        }
    }
    payload += "r0\n";

    std::string hex;
    hex.reserve(payload.size() * 2);
    for (uint8_t byte : payload) {
        hex_byte(hex, byte);
    }
    return hex;
}

std::string form_encode(std::string_view input) {
    constexpr char upper_hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (uint8_t c : input) {
        // urllib.quote_plus in Python 2 escapes '~' as well.
        if (is_alphanumeric(c) || c == '_' || c == '.' || c == '-') {
            encoded += char(c);
        } else if (c == ' ') {
            encoded += '+';
        } else {
            encoded += '%';
            encoded += upper_hex[c >> 4];
            encoded += upper_hex[c & 15];
        }
    }
    return encoded;
}

} // namespace

bool parse_endpoint(std::string_view url, Endpoint &endpoint) {
    while (!url.empty() && is_space(url.front())) {
        url.remove_prefix(1);
    }
    while (!url.empty() && is_space(url.back())) {
        url.remove_suffix(1);
    }
    constexpr std::string_view scheme = "http://";
    if (url.size() > 512 || url.substr(0, scheme.size()) != scheme) {
        return false;
    }
    url.remove_prefix(scheme.size());
    const auto path_at = url.find_first_of("/?");
    auto authority = url.substr(0, path_at);
    if (authority.empty() || authority.size() > 259 ||
        std::any_of(url.begin(), url.end(), [](uint8_t c) { return c <= 32 || c >= 127 || c == '#'; })) {
        return false;
    }
    const auto port_at = authority.find(':');
    const auto host = authority.substr(0, port_at);
    const auto port = port_at == std::string_view::npos ? std::string_view("80") : authority.substr(port_at + 1);
    if (host.empty() || host.size() > 253 ||
        std::any_of(host.begin(), host.end(), [](char c) { return !is_alphanumeric(c) && c != '-' && c != '.'; }) ||
        port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), is_digit)) {
        return false;
    }
    unsigned number = 0;
    for (char digit : port) {
        number = number * 10 + unsigned(digit - '0');
    }
    if (!number || number > 65535) {
        return false;
    }
    std::string path = path_at == std::string_view::npos ? "/" : std::string(url.substr(path_at));
    if (path.front() == '?') {
        path.insert(path.begin(), '/');
    }
    endpoint = {std::string(host), std::to_string(number), std::move(path)};
    return true;
}

std::string song_hash(const std::string &midi_path, const std::vector<uint8_t> &edited_midi) {
    Sha1 hash;
    if (!edited_midi.empty()) {
        hash.add(edited_midi.data(), edited_midi.size());
        return hash.finish();
    }

    auto *file = std::fopen(midi_path.c_str(), "rb");
    if (!file) {
        return {};
    }
    uint8_t buffer[1024];
    for (size_t count; (count = std::fread(buffer, 1, sizeof(buffer), file)) != 0;) {
        hash.add(buffer, count);
    }
    const bool failed = std::ferror(file);
    std::fclose(file);
    return failed ? std::string{} : hash.finish();
}

std::string request(const Endpoint &endpoint, const std::string &song_name,
                    const std::string &chart_hash, const Tables &tables) {
    const char separator = endpoint.path.find('?') == std::string::npos ? '?' : '&';
    const auto authority = endpoint.port == "80" ? endpoint.host : endpoint.host + ":" + endpoint.port;
    return "GET " + endpoint.path + separator + "scores=" + cereal_scores(tables) +
           "&version=1.3.110&songName=" + form_encode(song_name) + "&songHash=" + chart_hash +
           " HTTP/1.0\r\nHost: " + authority + "\r\nConnection: close\r\n\r\n";
}

Reply parse_reply(std::string_view body) {
    const auto separator = body.find(';');
    const bool success = body.substr(0, separator) == "True";
    if (separator == std::string_view::npos) {
        return {success, 0};
    }

    auto rank = body.substr(separator + 1);
    rank = rank.substr(0, rank.find(';'));
    while (!rank.empty() && is_space(rank.front())) {
        rank.remove_prefix(1);
    }
    while (!rank.empty() && is_space(rank.back())) {
        rank.remove_suffix(1);
    }
    const bool negative = !rank.empty() && rank.front() == '-';
    if (!rank.empty() && (negative || rank.front() == '+')) {
        rank.remove_prefix(1);
    }
    if (rank.empty() || !std::all_of(rank.begin(), rank.end(), is_digit)) {
        return {};
    }
    const unsigned limit = unsigned(INT_MAX) + unsigned(negative);
    unsigned value = 0;
    for (char c : rank) {
        const unsigned digit = unsigned(c - '0');
        if (value > (limit - digit) / 10) {
            return {};
        }
        value = value * 10 + digit;
    }
    const int64_t ordinal = negative ? -int64_t(value) : int64_t(value);
    return {success, int(ordinal)};
}

} // namespace dc::ranking
