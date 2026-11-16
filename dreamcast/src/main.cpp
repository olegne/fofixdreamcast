// SPDX-License-Identifier: GPL-2.0-or-later
#include "audio.hpp"
#include "chart.hpp"
#include "song_library.hpp"
#include "view.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <string>
#include <vector>
#include <arch/arch.h>
#include <arch/irq.h>
#include <arch/timer.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/maple/keyboard.h>
#include <dc/pvr.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/sound.h>
#include <dc/vblank.h>
#include <kos/init.h>

extern "C" {
volatile uint8_t dc_gameplay_active = 0;
volatile uint32_t dc_gems_hit = 0;
volatile uint32_t dc_targets_hit = 0;
volatile uint32_t dc_targets_missed = 0;
volatile uint32_t dc_overstrums = 0;
volatile uint32_t dc_score = 0;
volatile int32_t dc_event_time_ms = 0;
volatile int32_t dc_last_hit_chart_ms = -1;
}

namespace {

void record_round(const dc::Round &round, int time_ms) {
    dc_gems_hit = round.gem_hits;
    dc_targets_hit = round.hits;
    dc_targets_missed = round.misses;
    dc_overstrums = round.overstrums;
    dc_score = round.score;
    dc_event_time_ms = time_ms;
    dc_last_hit_chart_ms = round.hits ? round.last_hit_ms - round.last_hit_offset_ms : -1;
}

enum Effect : unsigned {
    accept, back, select1, select2, select3, start_song, miss1, miss2, miss3, miss4, miss5, miss6,
    no_points, taunt1, taunt2, taunt3, taunt4, myhero, perfect1, perfect2, perfect3, effect_count
};

class SoundEffects {
public:
    void load() {
        if (snd_init() < 0) {
            return;
        }
        const auto left = snd_sfx_chn_alloc();
        const auto right = snd_sfx_chn_alloc();
        if (left >= 0 && right == left + 1) {
            miss_channel_ = left;
        } else {
            if (left >= 0) {
                snd_sfx_chn_free(left);
            }
            if (right >= 0) {
                snd_sfx_chn_free(right);
            }
        }
        constexpr const char *names[] = {
            "in", "out", "crunch1", "crunch2", "crunch3", "start",
            "fiba1", "fiba2", "fiba3", "fiba4", "fiba5", "fiba6",
            "jurgen1", "jurgen2", "jurgen3", "jurgen4", "jurgen5",
            "myhero", "perfect1", "perfect2", "perfect3"
        };
        static_assert(sizeof(names) / sizeof(names[0]) == effect_count);
        for (unsigned i = 0; i < effect_count; ++i) {
            char path[64];
            std::snprintf(path, sizeof(path), "/rd/fx/%s.wav", names[i]);
            samples_[i] = snd_sfx_load(path);
            if (samples_[i] == SFXHND_INVALID) {
                std::printf("FoFDC: missing sound %s\n", path);
            }
        }
    }

    void play(Effect effect) const {
        if (samples_[effect] != SFXHND_INVALID) {
            constexpr unsigned miss_volume = 64;  // 25%
            const bool miss = effect >= miss1 && effect <= miss6;
            if (miss && miss_channel_ >= 0) {
                // GuitarScene uses one mixer channel and replaces the previous miss effect.
                snd_sfx_stop(miss_channel_);
                snd_sfx_stop(miss_channel_ + 1);
                snd_sfx_play_chn(miss_channel_, samples_[effect], miss_volume, 128);
            } else {
                snd_sfx_play(samples_[effect], miss ? miss_volume : 255, 128);
            }
        }
    }

private:
    std::array<sfxhnd_t, effect_count> samples_{};
    int miss_channel_ = -1;
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
            constexpr const char *names[] = {"pose.svg", "keyboard.svg", "esc.svg"};
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

enum InputButton : unsigned {
    nav_up = 1u << 0, nav_down = 1u << 1, nav_left = 1u << 2, nav_right = 1u << 3,
    select_option = 1u << 4, cancel_option = 1u << 5, pause_song = 1u << 6,
    pick_up = 1u << 7, pick_down = 1u << 8, pick_extra = 1u << 9
};

struct Input {
    dc::InputDevice device = dc::InputDevice::none;
    unsigned buttons = 0;
    uint8_t frets = 0;
};

Input read_keyboard(const kbd_state_t &keys) {
    Input input{dc::InputDevice::keyboard};
    for (unsigned fret = 0; fret < 5; ++fret) {
        if (keys.key_states[KBD_KEY_F1 + fret].is_down) {
            input.frets |= 1u << fret;
        }
    }
    constexpr struct {
        kbd_key_t key;
        unsigned buttons;
    } bindings[] = {
        {KBD_KEY_UP, nav_up}, {KBD_KEY_DOWN, nav_down},
        {KBD_KEY_LEFT, nav_left}, {KBD_KEY_RIGHT, nav_right},
        {KBD_KEY_ENTER, select_option | pick_down}, {KBD_KEY_ESCAPE, cancel_option | pause_song}
    };
    for (const auto &binding : bindings) {
        if (keys.key_states[binding.key].is_down) {
            input.buttons |= binding.buttons;
        }
    }
    if (keys.cond.modifiers.raw & KBD_MOD_RSHIFT) {
        input.buttons |= pick_up;
    }
    return input;
}

Input read_input() {
    if (const auto *keys = kbd_get_state(maple_enum_type(0, MAPLE_FUNC_KEYBOARD))) {
        return read_keyboard(*keys);
    }

    auto *controller = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    const auto *pad = controller ? static_cast<cont_state_t *>(maple_dev_status(controller)) : nullptr;
    if (!pad) {
        return {};
    }
    Input input{dc::InputDevice::controller};
    constexpr struct {
        unsigned key;
        unsigned buttons;
    } bindings[] = {
        {CONT_DPAD_UP, nav_up | pick_up}, {CONT_DPAD_DOWN, nav_down | pick_down},
        {CONT_DPAD_LEFT, nav_left}, {CONT_DPAD_RIGHT, nav_right},
        {CONT_A, select_option | pick_extra}, {CONT_START, select_option | pause_song},
        {CONT_B, cancel_option}
    };
    for (const auto &binding : bindings) {
        if (pad->buttons & binding.key) {
            input.buttons |= binding.buttons;
        }
    }
    input.frets = (pad->ltrig >= 96 ? 1u << 0 : 0) | (pad->buttons & CONT_X ? 1u << 1 : 0) |
                  (pad->buttons & CONT_Y ? 1u << 2 : 0) | (pad->buttons & CONT_B ? 1u << 3 : 0) |
                  (pad->rtrig >= 96 ? 1u << 4 : 0);
    return input;
}

struct KeyboardReport {
    Input state;
    uint32_t timestamp;
    unsigned pressed;
    uint8_t pressed_frets;
};

class KeyboardReports {
public:
    static void key_event(maple_device_t *dev, kbd_key_t, key_state_t, kbd_mods_t, kbd_leds_t, void *data) {
        static_cast<KeyboardReports *>(data)->capture(dev);
    }

    static void vblank(uint32_t, void *data) {
        static_cast<KeyboardReports *>(data)->capture(maple_enum_type(0, MAPLE_FUNC_KEYBOARD));
    }

    void capture(maple_device_t *dev) {
        if (dev != maple_enum_type(0, MAPLE_FUNC_KEYBOARD)) {
            return;
        }
        const auto *keys = kbd_get_state(dev);
        if (!keys) {
            return;
        }
        const auto state = read_keyboard(*keys);
        if (state.buttons == previous_.buttons && state.frets == previous_.frets) {
            return;
        }
        const auto pressed = state.buttons & ~previous_.buttons;
        const auto pressed_frets = uint8_t(state.frets & ~previous_.frets);
        previous_ = state;
        if (written_ - read_ == reports_.size()) {
            ++read_;
            ++overflow;
        }
        reports_[written_++ % reports_.size()] = {state, uint32_t(timer_ms_gettime64()), pressed, pressed_frets};
    }

    void reset(const Input &state) {
        const auto irq = irq_disable();
        read_ = written_;
        previous_ = state;
        irq_restore(irq);
    }

    bool pop(KeyboardReport &report) {
        const auto irq = irq_disable();
        const bool available = read_ != written_;
        if (available) {
            report = reports_[read_++ % reports_.size()];
        }
        irq_restore(irq);
        return available;
    }

    unsigned overflow = 0;

private:
    Input previous_{dc::InputDevice::keyboard};
    std::array<KeyboardReport, 32> reports_{};
    unsigned read_ = 0;
    unsigned written_ = 0;
};

unsigned move_list(unsigned current, unsigned count, int direction) {
    return (current + count + direction) % count;
}

struct FrameTiming {
    uint64_t last = 0;
    unsigned max_us = 0;
    unsigned over_25_ms = 0;
    unsigned over_50_ms = 0;
    unsigned frames = 0;

    void sample() {
        const auto now = timer_us_gettime64();
        if (last) {
            const auto delta = static_cast<unsigned>(now - last);
            max_us = std::max(max_us, delta);
            over_25_ms += delta > 25000;
            over_50_ms += delta > 50000;
        }
        last = now;
        ++frames;
    }
};

} // namespace

int main() {
    dc::View view;
    if (!view.init()) {
        std::puts("FoFDC: could not load the classic renderer");
        return 1;
    }

    SoundEffects sounds;
    sounds.load();
    KeyboardReports keyboard_reports;
    kbd_set_event_handler(KeyboardReports::key_event, &keyboard_reports);
    const auto vblank_handle = vblank_handler_add(KeyboardReports::vblank, &keyboard_reports);
    std::printf("FoFDC: Maple report sampling vblank=%d\n", vblank_handle >= 0);
    const auto tutorial = read_tutorial_cues();
    const auto songs = dc::song_library();
    dc::SongChart song;
    dc::Round round;
    dc::Frame frame;
    std::string load_error;
    bool training = false;
    bool settings_in_pause = false;
    unsigned song_index = 1;
    unsigned last_regular_song = 1;
    unsigned previous_buttons = 0;
    Input processed_input;
    int processed_time = 0;
    int chart_delay_ms = 0;
    uint64_t countdown_start = 0;
    unsigned countdown_ms = 0;
    uint64_t pause_frame_ms = 0;
    bool closing_pause = false;
    uint64_t round_start_ms = 0;
    bool error_to_main = false;
    FrameTiming timing;
    frame.delay_ms = 100;
    frame.difficulty = 2; // Player.py defaults to Easy.

    const auto to_setlist = [&] {
        audio.stop();
        frame.countdown = 0;
        training = false;
        if (songs.size() < 2) {
            frame.screen = dc::Screen::error;
            frame.error = "No songs on the disc.";
            error_to_main = true;
            return;
        }
        song_index = last_regular_song;
        frame.screen = dc::Screen::setlist;
        frame.selected = song_index;
    };
    const auto to_main = [&] {
        audio.stop();
        frame.countdown = 0;
        frame.screen = dc::Screen::menu;
        frame.selected = training ? 1 : 0;
    };
    const auto load_song = [&] {
        const auto &entry = songs[song_index];
        chart_delay_ms = entry.delay_ms;
        frame.title = entry.title.c_str();
        frame.artist = entry.artist.c_str();
        error_to_main = training;
        if (!dc::load_song_chart(entry.midi.c_str(), song, load_error)) {
            frame.screen = dc::Screen::error;
            frame.error = load_error.c_str();
            std::printf("FoFDC: %s\n", frame.error);
            return;
        }
        frame.available = 0;
        for (unsigned i = 0; i < song.guitar.size(); ++i) {
            if (!song.guitar[i].empty()) {
                frame.available |= 1u << i;
            }
        }
        if (!frame.available) {
            frame.screen = dc::Screen::error;
            frame.error = "No supported notes in this chart.";
            return;
        }
        frame.selected = (frame.available & (1u << frame.difficulty)) ? frame.difficulty :
                         31 - __builtin_clz(frame.available);
        frame.screen = dc::Screen::difficulty;
        std::printf("FoFDC: loaded %s (A=%u M=%u E=%u S=%u)\n", entry.title.c_str(),
                    unsigned(song.guitar[0].size()), unsigned(song.guitar[1].size()),
                    unsigned(song.guitar[2].size()), unsigned(song.guitar[3].size()));
    };
    const auto begin_countdown = [&] {
        audio.stop();
        round = dc::Round{};
        view.reset_effects();
        frame.gems = 0;
        for (const auto &target : song.guitar[frame.difficulty]) {
            frame.gems += dc::fret_count(target.frets);
        }
        countdown_ms = 8 * song.initial_us_per_beat / 1000;
        frame.screen = dc::Screen::countdown;
        frame.time_ms = -int(countdown_ms) - frame.delay_ms - chart_delay_ms;
        frame.countdown = 8;
        frame.duration_ms = 0;
        countdown_start = timer_ms_gettime64();
        timing = {};
        keyboard_reports.overflow = 0;
        sounds.play(start_song);
    };
    const auto begin_audio = [&] {
        frame.countdown = 0;
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
        frame.time_ms = -frame.delay_ms - chart_delay_ms;
        processed_input = read_input();
        processed_time = frame.time_ms;
        keyboard_reports.reset(processed_input);
        frame.screen = dc::Screen::play;
        std::printf("FoFDC: playing difficulty=%u goals=%u gems=%u delay=%d\n", frame.difficulty,
                    unsigned(song.guitar[frame.difficulty].size()), frame.gems, frame.delay_ms + chart_delay_ms);
        std::printf("FoFDC: memory heap_used=%u vram_free=%u aica_free=%u\n",
                    unsigned(mallinfo().uordblks), unsigned(pvr_mem_available()), unsigned(snd_mem_available()));
    };
    const auto resume_audio = [&] {
        if (audio.playing() || audio.resume()) {
            frame.screen = dc::Screen::play;
            frame.time_ms = audio.position_ms() - frame.delay_ms - chart_delay_ms;
            processed_input = read_input();
            processed_time = frame.time_ms;
            keyboard_reports.reset(processed_input);
            timing.last = 0;
            std::printf("FoFDC: resumed %d ms\n", audio.position_ms());
        } else {
            frame.screen = dc::Screen::error;
            frame.error = audio.error();
        }
    };
    const auto show_pause = [&] {
        frame.screen = dc::Screen::pause;
        frame.selected = 0;
        frame.pause_visibility = 0.0f;
        pause_frame_ms = timer_ms_gettime64();
        closing_pause = false;
        std::printf("FoFDC: pause opened chart=%d countdown=%.3f pcm=%d\n",
                    frame.time_ms, double(frame.countdown), frame.countdown > 0.0f ? 0 : audio.position_ms());
    };
    const auto toggle = [&](int steps) {
        if (steps) {
            sounds.play(static_cast<Effect>(select1 + std::rand() % 3));
        }
    };

    const auto resume_menu_music = [&] {
        audio.poll();
        if (!audio.playing() && !audio.start("/rd/menu.ogg")) {
            std::printf("FoFDC: menu audio error: %s\n", audio.error());
        }
    };

    for (;;) {
        const auto input = read_input();
        const bool same_device = input.device == frame.input;
        unsigned pressed = same_device && input.device == dc::InputDevice::controller ?
                           input.buttons & ~previous_buttons : 0;
        previous_buttons = input.buttons;
        unsigned menu_frets = 0;
        if (frame.screen != dc::Screen::play || input.device != dc::InputDevice::keyboard) {
            KeyboardReport report;
            while (keyboard_reports.pop(report)) {
                if (input.device == dc::InputDevice::keyboard) {
                    pressed |= report.pressed;
                    menu_frets |= report.pressed_frets;
                }
            }
        }
        frame.held = input.frets;
        frame.input = input.device;
        const bool confirm = (pressed & select_option) || (menu_frets & (1u << 0));
        const bool cancel = (pressed & cancel_option) || (menu_frets & (1u << 1));
        const int step = bool(pressed & nav_down) - bool(pressed & nav_up) +
                         (frame.input == dc::InputDevice::keyboard && (pressed & pick_up) ? 1 : 0);
        const int adjust = bool((pressed & nav_right) || (menu_frets & (1u << 3))) -
                           bool((pressed & nav_left) || (menu_frets & (1u << 2)));
        const auto &chart = song.guitar[frame.difficulty];
        frame.caption = nullptr;
        frame.tutorial_picture = -1;

        switch (frame.screen) {
            case dc::Screen::menu:
                resume_menu_music();
                frame.selected = move_list(frame.selected, 6, step);
                toggle(step);
                if (confirm) {
                    sounds.play(accept);
                    if (frame.selected == 5) {
                        arch_menu();
                    } else if (frame.selected == 3) {
                        settings_in_pause = false;
                        frame.screen = dc::Screen::settings;
                        frame.selected = 0;
                    } else if (frame.selected == 0) {
                        to_setlist();
                    } else if (frame.selected == 1) {
                        training = true;
                        song_index = 0;
                        load_song();
                        if (frame.screen == dc::Screen::difficulty) {
                            frame.difficulty = (frame.available & 2) ? 1 : frame.selected;
                            begin_countdown();
                        }
                    } else {
                        audio.stop();
                        error_to_main = true;
                        frame.screen = dc::Screen::error;
                        frame.error = "This menu is in development.";
                    }
                } else if (cancel) {
                    arch_menu();
                }
                break;
            case dc::Screen::settings:
                if (!settings_in_pause) {
                    resume_menu_music();
                }
                frame.selected = move_list(frame.selected, 3, step);
                toggle(step || adjust);
                if (frame.selected == 0) {
                    frame.delay_ms = std::clamp(frame.delay_ms + 10 * adjust, -250, 250);
                } else if (frame.selected == 1) {
                    frame.board_speed = std::clamp(frame.board_speed + 5 * adjust, 50, 200);
                }
                if (cancel || (confirm && frame.selected == 2)) {
                    frame.selected = settings_in_pause ? 0 : 3;
                    frame.screen = settings_in_pause ? dc::Screen::pause : dc::Screen::menu;
                    sounds.play(back);
                }
                break;
            case dc::Screen::setlist:
                frame.selected = 1 + move_list(frame.selected - 1, songs.size() - 1, step);
                toggle(step);
                if (confirm) {
                    song_index = last_regular_song = frame.selected;
                    sounds.play(accept);
                    load_song();
                } else if (cancel) {
                    sounds.play(back);
                    to_main();
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
                    begin_countdown();
                } else if (cancel) {
                    sounds.play(back);
                    to_setlist();
                }
                break;
            case dc::Screen::countdown:
                if (pressed & pause_song) {
                    show_pause();
                }
                break;
            case dc::Screen::play:
                break;
            case dc::Screen::pause: {
                const auto now = timer_ms_gettime64();
                const float transition = (now - pause_frame_ms) / 512.0f;
                pause_frame_ms = now;
                if (closing_pause) {
                    frame.pause_visibility = std::max(0.0f, frame.pause_visibility - transition);
                    if (frame.pause_visibility == 0.0f) {
                        chart_delay_ms = 0; // GuitarScene.resumeGame reloads the global A/V delay.
                        if (frame.countdown > 0.0f) {
                            frame.screen = dc::Screen::countdown;
                            std::printf("FoFDC: returned to countdown=%.3f\n", double(frame.countdown));
                        } else {
                            resume_audio();
                        }
                    }
                    break;
                }
                frame.pause_visibility = std::min(1.0f, frame.pause_visibility + transition);
                frame.selected = move_list(frame.selected, 5, step);
                toggle(step);
                if (cancel || (frame.input == dc::InputDevice::controller && confirm && frame.selected == 0)) {
                    sounds.play(cancel ? back : accept);
                    closing_pause = true;
                    std::printf("FoFDC: pause closing countdown=%.3f pcm=%d\n",
                                double(frame.countdown), frame.countdown > 0.0f ? 0 : audio.position_ms());
                } else if (confirm && frame.selected == 0) {
                    sounds.play(accept);
                } else if (confirm && frame.selected == 1) {
                    chart_delay_ms = 0;
                    begin_countdown();
                } else if (confirm && frame.selected == 2) {
                    to_setlist();
                } else if (confirm && frame.selected == 3) {
                    settings_in_pause = true;
                    frame.screen = dc::Screen::settings;
                    frame.selected = 0;
                } else if (confirm) {
                    to_main();
                }
                break;
            }
            case dc::Screen::results:
            case dc::Screen::failed:
                frame.selected = move_list(frame.selected, 3, step);
                toggle(step);
                if (cancel || (confirm && frame.selected == 2)) {
                    to_main();
                } else if (confirm && frame.selected == 1) {
                    to_setlist();
                } else if (confirm) {
                    chart_delay_ms = songs[song_index].delay_ms;
                    begin_countdown();
                }
                break;
            case dc::Screen::error:
                if (cancel || confirm) {
                    error_to_main ? to_main() : to_setlist();
                }
                break;
        }

        const bool pause_layer = frame.screen == dc::Screen::pause ||
                                 (frame.screen == dc::Screen::settings && settings_in_pause);
        const bool interactive = frame.screen == dc::Screen::play;
        if (frame.countdown > 0.0f) {
            const int remaining = std::max(0, int(countdown_ms) - int(timer_ms_gettime64() - countdown_start));
            frame.countdown = remaining * 1000.0f / song.initial_us_per_beat;
            frame.time_ms = -remaining - frame.delay_ms - chart_delay_ms;
            if (remaining == 0) {
                const auto overlay = frame.screen;
                begin_audio();
                if (pause_layer && frame.screen == dc::Screen::play) {
                    frame.screen = overlay;
                    std::printf("FoFDC: countdown completed under pause\n");
                }
            }
        } else if (interactive || (pause_layer && audio.playing())) {
            audio.poll();
            frame.time_ms = audio.position_ms() - frame.delay_ms - chart_delay_ms;
            const dc::Rules rules{60000000.0f / song.initial_us_per_beat,
                                  frame.delay_ms + chart_delay_ms};
            if (audio.error()) {
                frame.screen = dc::Screen::error;
                frame.error = audio.error();
            } else if (!audio.playing()) {
                round.finish(chart, frame.time_ms, rules);
                const unsigned stars = round.rating(frame.gems);
                if (!round.score) {
                    sounds.play(no_points);
                } else if (round.gem_hits * 100 >= frame.gems * 99) {
                    sounds.play(myhero);
                } else if (stars <= 1) {
                    sounds.play(static_cast<Effect>(taunt1 + std::rand() % 4));
                } else if (stars == 5) {
                    sounds.play(static_cast<Effect>(perfect1 + std::rand() % 3));
                }
                frame.selected = 0;
                frame.screen = dc::Screen::results;
                std::printf("FoFDC: end pcm=%d elapsed=%llu frames=%u hit=%u miss=%u extra=%u score=%u\n",
                            audio.position_ms(),
                            static_cast<unsigned long long>(timer_ms_gettime64() - round_start_ms),
                            timing.frames, round.hits, round.misses, round.overstrums, round.score);
                std::printf("FoFDC: gems=%u/%u stars=%u frame_max_us=%u over25=%u over50=%u\n",
                            round.gem_hits, frame.gems, stars, timing.max_us, timing.over_25_ms, timing.over_50_ms);
                std::printf("FoFDC: input_queue_overflow=%u\n", keyboard_reports.overflow);
            } else {
                if (songs[song_index].tutorial) {
                    tutorial_cue(frame, tutorial);
                }
                const auto record_pick = [&](dc::Pick result) {
                    audio.guitar_gain(round.guitar_muted ? 0.0f : 1.0f);
                    if (result == dc::Pick::hit) {
                        view.record_pick(round);
                    } else if (result == dc::Pick::bad) {
                        sounds.play(static_cast<Effect>(miss1 + std::rand() % 6));
                    }
                };
                const auto process_input = [&](const Input &state, int when) {
                    if (state.device == processed_input.device) {
                        const auto edges = state.buttons & ~processed_input.buttons;
                        if (edges & pause_song) {
                            return false;
                        }
                        if (state.frets != processed_input.frets) {
                            record_pick(round.fret_edge(chart, when, state.frets, rules,
                                                       (processed_input.frets & ~state.frets) != 0));
                        }
                        for (const auto pick : {pick_up, pick_down, pick_extra}) {
                            if (edges & pick) {
                                record_pick(round.pick(chart, when, state.frets, rules, false));
                            }
                        }
                    }
                    processed_input = state;
                    processed_time = when;
                    return true;
                };
                bool keep_playing = true;
                if (!interactive) {
                    processed_input = {};
                    processed_time = frame.time_ms;
                } else if (input.device == dc::InputDevice::keyboard) {
                    KeyboardReport report;
                    while (keep_playing && keyboard_reports.pop(report)) {
                        const auto now = uint32_t(timer_ms_gettime64());
                        const auto age = std::min<uint32_t>(now - report.timestamp, 1000);
                        keep_playing = process_input(report.state,
                                                     std::max(processed_time, frame.time_ms - int(age)));
                    }
                    if (keep_playing && (vblank_handle < 0 || processed_input.device != input.device)) {
                        keep_playing = process_input(input, frame.time_ms);
                    }
                } else {
                    keep_playing = process_input(input, frame.time_ms);
                }
                record_pick(round.advance(chart, frame.time_ms, processed_input.frets, rules));
                if (keep_playing) {
                    timing.sample();
                } else if (audio.pause()) {
                    show_pause();
                    std::printf("FoFDC: paused at %d ms\n", audio.position_ms());
                } else {
                    frame.screen = dc::Screen::error;
                    frame.error = audio.error();
                }
            }
        }

        dc_gameplay_active = frame.screen == dc::Screen::play;
        record_round(round, frame.time_ms);
        view.draw(frame, song, round, songs);
    }
}
