// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio.hpp"
#include "chart.hpp"
#include "song_library.hpp"
#include "view.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <arch/arch.h>
#include <arch/timer.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/sound.h>
#include <kos/init.h>

namespace {

constexpr unsigned strum_buttons = CONT_DPAD_UP | CONT_DPAD_DOWN | CONT_A;
constexpr unsigned countdown_ms = 3000;

enum Effect : unsigned {
    accept, back, select1, select2, select3, power, start_song, failed_song, passed_song, ding,
    lost, power_ready, miss1, miss2, miss3, miss4, miss5, miss6, effect_count
};

class SoundEffects {
public:
    void load() {
        if (snd_init() < 0) {
            return;
        }
        constexpr const char *names[] = {
            "accept1", "back1", "select1", "select2", "select3", "staractivate", "start",
            "failsound", "rocksound", "starding", "stardeactivate", "starpowerready", "guitscw1", "guitscw2",
            "guitscw3", "guitscw4", "guitscw5", "guitscw6"
        };
        static_assert(sizeof(names) / sizeof(names[0]) == effect_count);
        for (unsigned i = 0; i < effect_count; ++i) {
            char path[64];
            std::snprintf(path, sizeof(path), "/rd/fx/%s.wav", names[i]);
            samples_[i] = snd_sfx_load(path);
            if (samples_[i] == SFXHND_INVALID) {
                std::printf("FoFiXDC: missing sound %s\n", path);
            }
        }
    }

    void play(Effect effect) const {
        if (samples_[effect] != SFXHND_INVALID) {
            snd_sfx_play(samples_[effect], 204, 128);
        }
    }

private:
    std::array<sfxhnd_t, effect_count> samples_{};
};

struct Cue {
    int start, duration, picture;
    std::string text;
};

std::vector<Cue> read_tutorial_cues() {
    std::vector<Cue> cues;
    auto *script = std::fopen("/rd/tutorial/script.txt", "r");
    if (!script) {
        return cues;
    }
    char line[256];
    while (std::fgets(line, sizeof(line), script)) {
        int start, duration, offset;
        char type[8];
        if (std::sscanf(line, "%d %d %7s %n", &start, &duration, type, &offset) != 3) {
            continue;
        }
        auto *content = line + offset;
        content[std::strcspn(content, "\r\n")] = '\0';
        if (std::strcmp(type, "text") == 0) {
            cues.push_back({start, duration, -1, content});
        } else if (std::strcmp(type, "pic") == 0) {
            constexpr const char *names[] = {"pose.png", "keyboard.png", "esc.png"};
            for (int i = 0; i < 3; ++i) {
                if (std::strcmp(content, names[i]) == 0) {
                    cues.push_back({start, duration, i, {}});
                }
            }
        }
    }
    std::fclose(script);
    return cues;
}

void tutorial_cue(dc::Frame &frame, const std::vector<Cue> &cues) {
    for (const auto &cue : cues) {
        const int elapsed = frame.time_ms - cue.start;
        if (elapsed < 0 || elapsed > cue.duration) {
            continue;
        }
        if (cue.picture < 0) {
            frame.caption = cue.text.c_str();
        } else {
            const float in = std::min(1.0f, elapsed / 500.0f);
            const float out = std::min(1.0f, (cue.duration - elapsed) / 500.0f);
            frame.tutorial_picture = cue.picture;
            const float blend = 1.0f - in * out;
            frame.picture_fade = blend * blend;
        }
    }
}

static dc::Audio audio;

uint8_t read_frets(const cont_state_t &pad) {
    return (pad.ltrig >= 96 ? 1u << 0 : 0) | (pad.buttons & CONT_X ? 1u << 1 : 0) |
           (pad.buttons & CONT_Y ? 1u << 2 : 0) | (pad.buttons & CONT_B ? 1u << 3 : 0) |
           (pad.rtrig >= 96 ? 1u << 4 : 0);
}

unsigned move_list(unsigned current, unsigned count, int direction) {
    return (current + count + direction) % count;
}

} // namespace

int main() {
    dc::View view;
    if (!view.init()) {
        std::puts("FoFiXDC: could not load the MegaLight V4 renderer");
        return 1;
    }

    SoundEffects sounds;
    sounds.load();
    const auto tutorial = read_tutorial_cues();
    const auto songs = dc::song_library();
    dc::SongChart song;
    dc::Round round;
    dc::Frame frame;
    std::string load_error;
    unsigned song_index = 0;
    bool training = false;
    unsigned previous_buttons = 0;
    uint8_t previous_held = 0;
    uint64_t countdown_start = 0;
    uint64_t round_start_ms = 0;
    unsigned rendered_frames = 0;
    unsigned last_stars = 0;

    const auto to_setlist = [&] {
        audio.stop();
        frame.screen = dc::Screen::setlist;
        frame.selected = song_index;
    };
    const auto load_song = [&] {
        const auto &entry = songs[song_index];
        frame.title = entry.title.c_str();
        frame.artist = entry.artist.c_str();
        if (!dc::load_song_chart(entry.midi.c_str(), song, load_error)) {
            frame.screen = dc::Screen::error;
            frame.error = load_error.c_str();
            std::printf("FoFiXDC: %s\n", frame.error);
            return;
        }
        frame.available = 0;
        for (unsigned i = 0; i < song.guitar.size(); ++i) {
            if (!song.guitar[i].empty()) {
                frame.available |= 1u << i;
            }
        }
        frame.selected = __builtin_ctz(frame.available);
        frame.screen = dc::Screen::difficulty;
        std::printf("FoFiXDC: loaded %s (X=%u H=%u M=%u E=%u)\n", entry.title.c_str(),
                    unsigned(song.guitar[0].size()), unsigned(song.guitar[1].size()),
                    unsigned(song.guitar[2].size()), unsigned(song.guitar[3].size()));
    };
    const auto begin_countdown = [&] {
        audio.stop();
        round = dc::Round{};
        frame.screen = dc::Screen::countdown;
        frame.time_ms = -int(countdown_ms);
        frame.duration_ms = 0;
        countdown_start = timer_ms_gettime64();
        rendered_frames = 0;
        last_stars = 0;
        sounds.play(start_song);
    };
    const auto begin_audio = [&] {
        const auto &entry = songs[song_index];
        if (!audio.start(entry.backing.c_str(), entry.guitar.empty() ? nullptr : entry.guitar.c_str())) {
            frame.screen = dc::Screen::error;
            frame.error = audio.error();
            return;
        }
        round_start_ms = timer_ms_gettime64();
        int end_ms = 0;
        for (const auto &target : song.guitar[frame.difficulty]) {
            end_ms = std::max(end_ms, *std::max_element(target.ends.begin(), target.ends.end()));
        }
        frame.duration_ms = (end_ms + 1500) / 1000 * 1000;
        frame.time_ms = -frame.delay_ms - entry.delay_ms;
        frame.screen = dc::Screen::play;
    };
    const auto resume = [&] {
        if (audio.resume()) {
            frame.screen = dc::Screen::play;
            std::printf("FoFiXDC: resumed at %d ms\n", audio.position_ms());
        } else {
            frame.screen = dc::Screen::error;
            frame.error = audio.error();
        }
    };
    const auto toggle = [&](int steps) {
        if (steps) {
            sounds.play(static_cast<Effect>(select1 + frame.selected % 3));
        }
    };

    for (;;) {
        auto *device = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        const auto *pad = device ? static_cast<cont_state_t *>(maple_dev_status(device)) : nullptr;
        const unsigned buttons = pad ? pad->buttons : 0;
        const unsigned pressed = buttons & ~previous_buttons;
        previous_buttons = buttons;
        frame.held = pad ? read_frets(*pad) : 0;
        frame.connected = pad != nullptr;
        const auto highest_fret = [](uint8_t held) { return held ? 32 - __builtin_clz(held) : 0; };
        const bool fret_changed = highest_fret(previous_held) != highest_fret(frame.held);
        previous_held = frame.held;
        const bool confirm = pressed & (CONT_A | CONT_START);
        const bool cancel = pressed & CONT_B;
        const int step = bool(pressed & CONT_DPAD_DOWN) - bool(pressed & CONT_DPAD_UP);
        const int adjust = bool(pressed & CONT_DPAD_RIGHT) - bool(pressed & CONT_DPAD_LEFT);
        const auto &chart = song.guitar[frame.difficulty];
        frame.caption = nullptr;
        frame.tutorial_picture = -1;

        switch (frame.screen) {
            case dc::Screen::menu:
                if (step) {
                    do {
                        frame.selected = move_list(frame.selected, 5, step);
                    } while (frame.selected == 1);
                }
                toggle(step);
                if (confirm) {
                    sounds.play(accept);
                    if (frame.selected == 4) {
                        arch_menu();
                    } else if (frame.selected == 3) {
                        frame.screen = dc::Screen::settings;
                        frame.selected = 0;
                    } else {
                        training = frame.selected == 2;
                        to_setlist();
                    }
                }
                break;
            case dc::Screen::settings:
                frame.selected = move_list(frame.selected, 3, step);
                toggle(step || adjust);
                if (frame.selected == 0) {
                    frame.delay_ms = std::clamp(frame.delay_ms + 10 * adjust, -250, 250);
                } else if (frame.selected == 1) {
                    frame.board_speed = std::clamp(frame.board_speed + 5 * adjust, 50, 200);
                }
                if (cancel || (confirm && frame.selected == 2)) {
                    frame.selected = 3;
                    frame.screen = dc::Screen::menu;
                    sounds.play(back);
                }
                break;
            case dc::Screen::setlist:
                frame.selected = move_list(frame.selected, songs.size(), step);
                toggle(step);
                if (confirm) {
                    song_index = frame.selected;
                    sounds.play(accept);
                    load_song();
                } else if (cancel) {
                    sounds.play(back);
                    frame.selected = training ? 2 : 0;
                    frame.screen = dc::Screen::menu;
                }
                break;
            case dc::Screen::difficulty:
                if (step) {
                    do {
                        frame.selected = move_list(frame.selected, 4, -step);
                    } while (!(frame.available & (1u << frame.selected)));
                }
                toggle(step);
                if (confirm) {
                    frame.difficulty = frame.selected;
                    frame.gems = 0;
                    for (const auto &target : song.guitar[frame.difficulty]) {
                        frame.gems += dc::fret_count(target.frets);
                    }
                    begin_countdown();
                } else if (cancel) {
                    sounds.play(back);
                    to_setlist();
                }
                break;
            case dc::Screen::countdown: {
                const int elapsed = int(timer_ms_gettime64() - countdown_start);
                frame.time_ms = elapsed - int(countdown_ms);
                frame.countdown = std::max(1, (-frame.time_ms + 999) / 1000);
                if (cancel) {
                    to_setlist();
                } else if (frame.time_ms >= 0) {
                    begin_audio();
                }
                break;
            }
            case dc::Screen::play: {
                audio.poll();
                frame.time_ms = audio.position_ms() - frame.delay_ms - songs[song_index].delay_ms;
                const dc::Rules rules{60000000.0f / song.us_per_beat(frame.time_ms)};
                if (audio.error()) {
                    frame.screen = dc::Screen::error;
                    frame.error = audio.error();
                } else if (!audio.playing()) {
                    round.finish(chart, frame.time_ms, rules);
                    sounds.play(passed_song);
                    frame.selected = 0;
                    frame.screen = dc::Screen::results;
                    std::printf("FoFiXDC: end pcm=%d elapsed=%llu frames=%u hit=%u miss=%u extra=%u score=%u\n",
                                audio.position_ms(),
                                static_cast<unsigned long long>(timer_ms_gettime64() - round_start_ms),
                                rendered_frames, round.hits, round.misses, round.overstrums, round.score);
                } else if (pressed & CONT_START) {
                    if (audio.pause()) {
                        frame.selected = 0;
                        frame.screen = dc::Screen::pause;
                        std::printf("FoFiXDC: paused at %d ms\n", audio.position_ms());
                    } else {
                        frame.screen = dc::Screen::error;
                        frame.error = audio.error();
                    }
                } else {
                    if (songs[song_index].tutorial) {
                        tutorial_cue(frame, tutorial);
                    }
                    const bool was_power = round.power_active;
                    const auto previous_power = round.power;
                    round.advance(chart, frame.time_ms, frame.held, rules);
                    if (pressed & CONT_DPAD_LEFT && round.activate_power()) {
                        sounds.play(power);
                    } else if (was_power && !round.power_active) {
                        sounds.play(lost);
                    }
                    const bool strum = pressed & strum_buttons;
                    const bool tap = !strum && fret_changed;
                    if (strum || tap) {
                        const auto pick = round.pick(chart, frame.time_ms, frame.held, rules, tap);
                        if (pick == dc::Pick::bad) {
                            sounds.play(static_cast<Effect>(miss1 + round.overstrums % 6));
                        }
                        if (round.power > previous_power && previous_power < 50 && round.power >= 50) {
                            sounds.play(power_ready);
                        }
                    }
                    const unsigned earned = round.rating(frame.gems, chart.size(), frame.time_ms, rules);
                    if (earned > last_stars) {
                        sounds.play(ding);
                    }
                    last_stars = earned;
                    const bool whammy = (buttons & CONT_DPAD_RIGHT) && round.sustain_frets;
                    const float miss_gain = audio.has_guitar() ? 0.2f : 0.9f;
                    audio.guitar_gain(whammy ? 0.0f : (round.guitar_muted ? miss_gain : 1.0f));
                    if (!training && round.rock <= 0.0f) {
                        audio.stop();
                        sounds.play(failed_song);
                        frame.selected = 0;
                        frame.screen = dc::Screen::failed;
                    }
                    ++rendered_frames;
                }
                break;
            }
            case dc::Screen::pause: {
                frame.selected = move_list(frame.selected, 3, step);
                toggle(step);
                frame.delay_ms = std::clamp(frame.delay_ms + 10 * adjust, -250, 250);
                if (cancel || (confirm && frame.selected == 0)) {
                    resume();
                } else if (confirm && frame.selected == 1) {
                    begin_countdown();
                } else if (confirm) {
                    to_setlist();
                }
                break;
            }
            case dc::Screen::results:
            case dc::Screen::failed:
                frame.selected = move_list(frame.selected, 2, step);
                toggle(step);
                if (cancel || (confirm && frame.selected == 1)) {
                    to_setlist();
                } else if (confirm) {
                    begin_countdown();
                }
                break;
            case dc::Screen::error:
                if (cancel || confirm) {
                    to_setlist();
                }
                break;
        }

        view.draw(frame, song, round, songs);
    }
}
