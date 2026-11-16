// SPDX-License-Identifier: GPL-2.0-or-later
#include "chart.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

namespace dc {
namespace {

constexpr size_t max_chart_bytes = 2 * 1024 * 1024;
constexpr size_t max_events = 65536;
constexpr int max_song_ms = 60 * 60 * 1000;

struct Reader {
    const uint8_t *current;
    const uint8_t *end;

    size_t remaining() const { return static_cast<size_t>(end - current); }

    const uint8_t *take(size_t count) {
        if (count > remaining()) {
            return nullptr;
        }
        const auto *start = current;
        current += count;
        return start;
    }

    bool varint(uint32_t &value) {
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const auto *byte = take(1);
            if (!byte) {
                return false;
            }
            value = (value << 7) | (*byte & 0x7f);
            if (!(*byte & 0x80)) {
                return true;
            }
        }
        return false;
    }
};

uint16_t be16(const uint8_t *data) {
    return (uint16_t(data[0]) << 8) | data[1];
}

uint32_t be32(const uint8_t *data) {
    return (uint32_t(data[0]) << 24) | (uint32_t(data[1]) << 16) |
           (uint32_t(data[2]) << 8) | data[3];
}

struct Tempo {
    uint32_t tick;
    uint32_t us_per_beat;
};

struct Gem {
    uint32_t tick;
    uint8_t frets;
};

bool is_guitar(std::string_view name) {
    // Names accepted by FoFiX for the standard guitar instrument.
    return name == "PART GUITAR" || name == "T1 GEMS" || name == "Click" ||
           name == "Midi Out";
}

bool read_track(Reader track, std::vector<Tempo> &tempos,
                std::vector<Gem> &gems, std::string_view &name) {
    uint32_t tick = 0;
    uint8_t running_status = 0;
    while (track.remaining()) {
        uint32_t delta;
        if (!track.varint(delta) || delta > std::numeric_limits<uint32_t>::max() - tick ||
            !track.remaining()) {
            return false;
        }
        tick += delta;

        uint8_t status = *track.current;
        if (status & 0x80) {
            track.take(1);
        } else {
            status = running_status;
            if (!status) {
                return false;
            }
        }

        if (status == 0xff) {
            const auto *type = track.take(1);
            uint32_t length;
            if (!type || !track.varint(length)) {
                return false;
            }
            const auto *payload = track.take(length);
            if (!payload) {
                return false;
            }
            if (*type == 0x2f) {
                return length == 0 && !track.remaining();
            }
            if (*type == 0x03 && name.empty()) {
                name = {reinterpret_cast<const char *>(payload), length};
            }
            if (*type == 0x51) {
                if (length != 3 || tempos.size() == max_events) {
                    return false;
                }
                const auto tempo = (uint32_t(payload[0]) << 16) |
                                   (uint32_t(payload[1]) << 8) | payload[2];
                if (!tempo) {
                    return false;
                }
                tempos.push_back({tick, tempo});
            }
            continue;
        }

        if (status == 0xf0 || status == 0xf7) {
            running_status = 0;
            uint32_t length;
            if (!track.varint(length) || !track.take(length)) {
                return false;
            }
            continue;
        }
        if (status >= 0xf0) {
            return false;
        }
        const auto command = status & 0xf0;
        const size_t length = command == 0xc0 || command == 0xd0 ? 1 : 2;
        const auto *data = track.take(length);
        if (!data || (data[0] & 0x80) || (data[length - 1] & 0x80)) {
            return false;
        }
        running_status = status;
        if (command == 0x90 && data[1] && data[0] >= 96 && data[0] <= 100) {
            if (gems.size() == max_events) {
                return false;
            }
            const auto fret = static_cast<uint8_t>(1u << (data[0] - 96));
            if (!gems.empty() && gems.back().tick == tick) {
                gems.back().frets |= fret;
            } else {
                gems.push_back({tick, fret});
            }
        }
    }
    return false;
}

bool parse_chart(const std::vector<uint8_t> &bytes, Chart &chart, std::string &error) {
    Reader file{bytes.data(), bytes.data() + bytes.size()};
    const auto *chunk = file.take(8);
    if (!chunk || std::memcmp(chunk, "MThd", 4) != 0) {
        error = "CABECALHO MIDI INVALIDO";
        return false;
    }
    const uint32_t header_size = be32(chunk + 4);
    const auto *header = file.take(header_size);
    if (!header || header_size < 6) {
        error = "CABECALHO MIDI INCOMPLETO";
        return false;
    }
    const auto format = be16(header);
    const auto tracks = be16(header + 2);
    const auto ppqn = be16(header + 4);
    if (format > 1 || !tracks || (format == 0 && tracks != 1) || !ppqn || (ppqn & 0x8000)) {
        error = "USE MIDI TIPO 0 OU 1 COM PPQN";
        return false;
    }

    std::vector<Gem> guitar;
    std::vector<Tempo> tempos;
    for (unsigned i = 0; i < tracks; ++i) {
        chunk = file.take(8);
        if (!chunk || std::memcmp(chunk, "MTrk", 4) != 0) {
            error = "PISTA MIDI INVALIDA";
            return false;
        }
        const uint32_t track_size = be32(chunk + 4);
        const auto *start = file.take(track_size);
        std::vector<Gem> gems;
        std::string_view name;
        if (!start || !read_track({start, start + track_size}, tempos, gems, name)) {
            error = "EVENTO MIDI INVALIDO";
            return false;
        }
        if (guitar.empty() && (is_guitar(name) || (tracks == 1 && name.empty()))) {
            guitar = std::move(gems);
        }
    }
    if (guitar.empty()) {
        error = "SEM NOTAS DE GUITARRA EXPERT";
        return false;
    }

    // Convert absolute ticks using the full tempo map, including changes inside the song.
    std::stable_sort(tempos.begin(), tempos.end(), [](const Tempo &a, const Tempo &b) {
        return a.tick < b.tick;
    });
    uint32_t current_tick = 0;
    uint32_t current_tempo = 500000;
    uint64_t scaled_time = 0;
    size_t tempo_index = 0;
    Chart parsed;
    parsed.reserve(guitar.size());
    for (const auto &gem : guitar) {
        while (tempo_index < tempos.size() && tempos[tempo_index].tick <= gem.tick) {
            const auto &tempo = tempos[tempo_index++];
            scaled_time += uint64_t(tempo.tick - current_tick) * current_tempo;
            current_tick = tempo.tick;
            current_tempo = tempo.us_per_beat;
        }
        const auto gem_time = scaled_time + uint64_t(gem.tick - current_tick) * current_tempo;
        const auto time_ms = gem_time / (uint32_t(ppqn) * 1000);
        if (time_ms > max_song_ms) {
            error = "MIDI ULTRAPASSA 60 MINUTOS";
            return false;
        }
        parsed.push_back({static_cast<int>(time_ms), gem.frets});
    }
    chart = std::move(parsed);
    return true;
}

} // namespace

bool load_chart(const char *path, Chart &chart, std::string &error) {
    auto *file = std::fopen(path, "rb");
    if (!file) {
        error = "NAO FOI POSSIVEL ABRIR NOTES.MID";
        return false;
    }
    const long size = std::fseek(file, 0, SEEK_END) == 0 ? std::ftell(file) : -1;
    if (size < 14 || static_cast<size_t>(size) > max_chart_bytes ||
        std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        error = "TAMANHO MIDI INVALIDO";
        return false;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    const auto read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);
    if (read != bytes.size()) {
        error = "ERRO DE LEITURA MIDI";
        return false;
    }
    return parse_chart(bytes, chart, error);
}

unsigned Round::multiplier() const {
    return 1 + std::min(streak / 10, 3u);
}

void Round::advance(const Chart &chart, int time_ms) {
    while (next < chart.size() && time_ms > chart[next].time_ms + hit_window_ms) {
        ++next;
        ++misses;
        streak = 0;
    }
}

bool Round::strum(const Chart &chart, int time_ms, uint8_t frets) {
    advance(chart, time_ms);
    if (next == chart.size() || time_ms < chart[next].time_ms - hit_window_ms ||
        frets != chart[next].frets) {
        ++overstrums;
        streak = 0;
        return false;
    }
    ++next;
    ++hits;
    ++streak;
    best_streak = std::max(streak, best_streak);
    score += 50 * static_cast<unsigned>(__builtin_popcount(frets)) * multiplier();
    return true;
}

void Round::finish(const Chart &chart) {
    misses += static_cast<unsigned>(chart.size() - next);
    next = chart.size();
}

} // namespace dc
