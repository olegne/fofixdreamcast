// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>

namespace dc {

class StageTransition {
public:
    bool advance(bool playing, uint64_t now) {
        // GuitarScene renders Stage with its previous curved visibility,
        // then stores the updated View-layer visibility for the next frame.
        const float hidden = 1.0f - layer_;
        visibility_ = 1.0f - hidden * hidden;
        const float step = previous_ms_ ? (now - previous_ms_) / 512.0f : 0.0f;
        previous_ms_ = now;
        layer_ = std::clamp(layer_ + (playing ? step : -step), 0.0f, 1.0f);
        slide_ = 1.0f - visibility_ * visibility_;
        if (slide_ > 0.01f) {
            opacity_ = visibility_;
        }
        return playing || visibility_ > 0.0f;
    }

    float visibility() const { return visibility_; }
    float opacity() const { return opacity_; }

    float x(float position) const {
        const float shift = slide_ > 0.01f && (position < -0.25f || position > 0.25f)
            ? (position < 0.0f ? -slide_ : slide_) * 640.0f : 0.0f;
        return 320.0f + position * 320.0f + shift;
    }

private:
    uint64_t previous_ms_ = 0;
    float layer_ = 0.0f;
    float visibility_ = 0.0f;
    float slide_ = 1.0f;
    float opacity_ = 0.0f;
};

} // namespace dc
