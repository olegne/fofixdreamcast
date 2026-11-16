// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>

namespace dc {

enum class CreditAlign { left, center, right };

struct CreditLine {
    const char *text;
    CreditAlign align;
    float scale;
    bool gold;
};

struct CreditsFrame {
    unsigned elapsed_ms = 0;
    float visibility = 0.0f;
};

constexpr unsigned credits_duration_ms = 35500;
constexpr unsigned credits_transition_ms = 512;

const std::array<CreditLine, 80> &credits_lines();

} // namespace dc
