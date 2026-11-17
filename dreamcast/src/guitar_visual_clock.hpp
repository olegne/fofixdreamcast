// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "chart.hpp"

namespace dc {

struct GuitarVisualFrame {
    float note_period_ms;
    float grid_beat;
    float scroll_beat;
};

class GuitarVisualClock {
public:
    void restart(bool same_scene) {
        if (!same_scene) {
            *this = {};
        }
        beat_ = 0;
        last_beat_ms_ = 0;
    }

    // Stage.run uses the period calculated by the previous Guitar.renderNotes.
    bool advance_stage(int time_ms) {
        stage_period_ms_ = render_period_ms_;
        const int beat = static_cast<int>(4.0f * time_ms / stage_period_ms_) / 4;
        if (beat <= beat_) {
            return false;
        }
        beat_ = beat;
        last_beat_ms_ = time_ms;
        return true;
    }

    GuitarVisualFrame render(int time_ms, const std::vector<TempoPoint> &tempos,
                             bool countdown, bool sustain_expired) {
        // The countdown calls Guitar.setBPM every tick. That method resets the
        // neck offset, while Guitar.run eases the render BPM toward its target.
        if (countdown) {
            base_beat_ = 0.0f;
        }
        if (!sustain_expired) {
            bpm_ += 0.03f * (target_bpm_ - bpm_);
        }
        const float grid_beat = (time_ms - last_tempo_ms_) / render_period_ms_;

        // The neck and bars render before Guitar.renderNotes changes the
        // period. Track.getEvents exposes 50 ms buckets with an exclusive end.
        render_period_ms_ = 60000.0f / bpm_;
        const int first_bucket = static_cast<int>((time_ms - 2.0f * render_period_ms_) / 50.0f);
        const int end_bucket = static_cast<int>((time_ms + 5.0f * render_period_ms_) / 50.0f);
        const float scroll_beat = grid_beat + base_beat_;
        while (tempo_index_ < tempos.size()) {
            const auto &tempo = tempos[tempo_index_];
            const float position = tempo.position_ms();
            const int bucket = tempo.time_ms / 50;
            if (position <= last_tempo_ms_ || bucket < first_bucket) {
                ++tempo_index_;
                continue;
            }
            if (bucket >= end_bucket ||
                (last_tempo_ms_ >= 0.0f && time_ms - position <= render_period_ms_)) {
                break;
            }
            base_beat_ += (position - last_tempo_ms_) / render_period_ms_;
            target_bpm_ = 60000000.0f / tempo.us_per_beat;
            last_tempo_ms_ = position;
            ++tempo_index_;
        }
        return {render_period_ms_, grid_beat, scroll_beat};
    }

    int last_beat_ms() const { return last_beat_ms_; }
    float stage_period_ms() const { return stage_period_ms_; }

private:
    float bpm_ = 50.0f;
    float target_bpm_ = 50.0f;
    float render_period_ms_ = 1200.0f;
    float stage_period_ms_ = 1200.0f;
    float last_tempo_ms_ = -1.0f;
    float base_beat_ = 0.0f;
    size_t tempo_index_ = 0;
    int beat_ = 0;
    int last_beat_ms_ = 0;
};
} // namespace dc
