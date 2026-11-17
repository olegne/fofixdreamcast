// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dc {

inline constexpr char sd_songs_root[] = "/sd/songs";

struct SongEntry {
    std::string title;
    std::string artist;
    std::string midi;
    std::string backing;
    std::string guitar;
    std::string rhythm;
    int delay_ms = 0;
    bool tutorial = false;
    uint32_t cassette_color = 0;
    bool blank_chart = false;
    int library_count = -1;
    std::string cassette_label = {};
};

std::vector<SongEntry> song_library();
std::vector<SongEntry> imported_song_sources();
const char *load_sd_song_library(std::vector<SongEntry> &songs, std::vector<SongEntry> &imports);

} // namespace dc
