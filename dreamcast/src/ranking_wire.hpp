// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "records.hpp"

#include <string_view>

namespace dc::ranking {

struct Endpoint {
    std::string host;
    std::string port;
    std::string path;
};

struct Reply {
    bool success = false;
    int ordinal = 0;
};

using Tables = std::array<ScoreBook::Table, 4>;

bool parse_endpoint(std::string_view url, Endpoint &endpoint);
std::string song_hash(const std::string &midi_path, const std::vector<uint8_t> &edited_midi);
std::string request(const Endpoint &endpoint, const std::string &song_name,
                    const std::string &chart_hash, const Tables &tables);
Reply parse_reply(std::string_view body);

} // namespace dc::ranking
