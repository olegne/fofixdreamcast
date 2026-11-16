// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>
#include <vector>

namespace dc {

struct SongEntry {
    std::string title;
    std::string artist;
    std::string midi;
    std::string backing;
    std::string guitar;
    int delay_ms = 0;
    bool tutorial = false;
};

std::vector<SongEntry> song_library();

} // namespace dc
