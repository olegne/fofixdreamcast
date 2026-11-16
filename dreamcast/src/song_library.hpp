// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dc {

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
};

std::vector<SongEntry> song_library();
std::vector<SongEntry> imported_song_sources();

} // namespace dc
