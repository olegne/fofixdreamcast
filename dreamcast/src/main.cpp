// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio.hpp"
#include "chart.hpp"
#include "view.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <arch/timer.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <kos/init.h>

namespace {

constexpr const char *chart_path = "/rd/notes.mid";
constexpr const char *audio_path = "/rd/song.ogg";
constexpr unsigned strum_buttons = CONT_DPAD_UP | CONT_DPAD_DOWN | CONT_A;

static dc::Audio audio;

uint8_t read_frets(const cont_state_t &pad) {
    return (pad.ltrig >= 96 ? 1u << 0 : 0) | (pad.buttons & CONT_X ? 1u << 1 : 0) |
           (pad.buttons & CONT_Y ? 1u << 2 : 0) | (pad.buttons & CONT_B ? 1u << 3 : 0) |
           (pad.rtrig >= 96 ? 1u << 4 : 0);
}

} // namespace

int main() {
    dc::View view;
    if (!view.init()) {
        std::puts("FoFiXDC: PVR init failed");
        return 1;
    }

    dc::Chart chart;
    dc::Round round;
    dc::Frame frame;
    std::string chart_error;
    const bool chart_loaded = dc::load_chart(chart_path, chart, chart_error);
    frame.retry_audio = chart_loaded;
    if (!chart_loaded) {
        frame.screen = dc::Screen::error;
        frame.error = chart_error.c_str();
        std::printf("FoFiXDC: %s\n", frame.error);
    } else {
        std::printf("FoFiXDC: %u guitar chords, last at %d ms\n",
                    static_cast<unsigned>(chart.size()), chart.back().time_ms);
    }

    unsigned previous_buttons = 0;
    int feedback_until = 0;
    uint64_t round_start_ms = 0;
    unsigned rendered_frames = 0;
    for (;;) {
        auto *device = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        const auto *pad = device ? static_cast<cont_state_t *>(maple_dev_status(device)) : nullptr;
        const unsigned buttons = pad ? pad->buttons : 0;
        const unsigned pressed = buttons & ~previous_buttons;
        previous_buttons = buttons;
        frame.held = pad ? read_frets(*pad) : 0;
        frame.connected = pad != nullptr;

        if (frame.screen == dc::Screen::play) {
            audio.poll();
            if (audio.error()) {
                frame.screen = dc::Screen::error;
                frame.error = audio.error();
            } else if (!audio.playing()) {
                round.finish(chart);
                frame.screen = dc::Screen::results;
                std::printf("FoFiXDC: end pcm=%d ms elapsed=%llu ms frames=%u hits=%u misses=%u "
                            "overstrums=%u score=%u\n", audio.position_ms(),
                            static_cast<unsigned long long>(timer_ms_gettime64() - round_start_ms),
                            rendered_frames, round.hits, round.misses, round.overstrums, round.score);
            } else {
                frame.time_ms = audio.position_ms() - frame.delay_ms;
                round.advance(chart, frame.time_ms);
                if (frame.time_ms >= feedback_until) {
                    frame.flash = 0;
                    frame.bad_strum = false;
                }
                if (pressed & strum_buttons) {
                    const bool hit = round.strum(chart, frame.time_ms, frame.held);
                    frame.flash = hit ? frame.held : 0;
                    frame.bad_strum = !hit;
                    feedback_until = frame.time_ms + 130;
                }
                ++rendered_frames;
            }
        } else if (chart_loaded) {
            const int step = 10 * (bool(pressed & CONT_DPAD_RIGHT) - bool(pressed & CONT_DPAD_LEFT));
            frame.delay_ms = std::clamp(frame.delay_ms + step, -250, 250);
            if (pressed & CONT_START) {
                round = {};
                frame.time_ms = -frame.delay_ms;
                frame.flash = 0;
                frame.bad_strum = false;
                feedback_until = frame.time_ms;
                rendered_frames = 0;
                if (audio.start(audio_path)) {
                    round_start_ms = timer_ms_gettime64();
                    frame.screen = dc::Screen::play;
                } else {
                    frame.screen = dc::Screen::error;
                    frame.error = audio.error();
                }
            }
        }

        view.draw(frame, chart, round);
    }
}
