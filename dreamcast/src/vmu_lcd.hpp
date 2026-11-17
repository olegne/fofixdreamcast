// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "vmu_playfield.hpp"

#include <cstdint>

namespace dc {

class VmuLcd {
public:
    // Check before rendering. A successful check permits one presentation.
    bool ready(uint64_t now_ms);
    void present(const VmuBitmap &bitmap);
    void suspend();

private:
    uint64_t next_refresh_ms_ = 0;
    uint32_t generation_ = 0;
    int port_ = -1;
    int unit_ = -1;
    bool active_ = false;
};

} // namespace dc
