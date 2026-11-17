// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "chart.hpp"

namespace dc {

// Maple LCD packet layout: bottom-right first, most significant bit first.
using VmuBitmap = std::array<uint8_t, 48 * 32 / 8>;

enum class VmuStatus { menu, paused, result, credits };

void render_vmu_playfield(VmuBitmap &bitmap, const Chart &chart, const Round &round,
                          int time_ms, uint8_t held_frets, bool lefty);
void render_vmu_status(VmuBitmap &bitmap, VmuStatus status, unsigned phase);

} // namespace dc
