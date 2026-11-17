// SPDX-License-Identifier: GPL-2.0-or-later
#include "chooser.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <utility>

namespace dc {
namespace {
constexpr char songs_root[] = "/cd/songs";

std::string parent_path(const std::string &path) {
    return path.substr(0, path.rfind('/'));
}

std::string containing_library(const SongEntry &song) {
    if (song.midi.compare(0, sizeof(songs_root), "/cd/songs/") == 0 ||
        song.midi.compare(0, sizeof(sd_songs_root), "/sd/songs/") == 0) {
        return parent_path(parent_path(song.midi));
    }
    constexpr char sd_imports_prefix[] = "/sd/imports/";
    if (song.midi.compare(0, sizeof(sd_imports_prefix) - 1, sd_imports_prefix) == 0) {
        return sd_songs_root;
    }
    return songs_root;
}

std::string trim(const std::string &s) {
    const auto start = s.find_first_not_of(" \t\r\n");
    return start == std::string::npos ? std::string{} : s.substr(start, s.find_last_not_of(" \t\r\n") - start + 1);
}

void read_folder(const std::string &path, SongEntry &folder) {
    folder.title = path.substr(path.rfind('/') + 1);
    folder.midi = path;
    if (auto *input = std::fopen((path + "/library.ini").c_str(), "r")) {
        char buffer[512];
        bool section = false;
        while (std::fgets(buffer, sizeof(buffer), input)) {
            const auto line = trim(buffer);
            if (!line.empty() && line[0] == '[') {
                section = line == "[library]";
            }
            const auto equals = line.find('=');
            if (!section || equals == std::string::npos) {
                continue;
            }
            const auto key = trim(line.substr(0, equals));
            const auto value = trim(line.substr(equals + 1));
            if (key == "name" && !value.empty()) {
                folder.title = value;
            } else if (key == "color" && value.size() == 7 && value[0] == '#') {
                char *end;
                const unsigned long color = std::strtoul(value.c_str() + 1, &end, 16);
                if (*end == 0 && color <= 0xffffff) {
                    folder.cassette_color = 0xff000000 | uint32_t(color);
                }
            }
        }
        std::fclose(input);
    }
}

std::string fold(std::string value) {
    for (size_t i = 0; i < value.size(); ++i) {
        const auto code = static_cast<unsigned char>(value[i]);
        if (code >= 'A' && code <= 'Z') {
            value[i] += 'a' - 'A';
        }
        if (code == 0xc3 && i + 1 < value.size()) {
            const auto second = static_cast<unsigned char>(value[i + 1]);
            if (second >= 0x80 && second <= 0x9e && second != 0x97) {
                value[i + 1] = static_cast<char>(second + 32);
            }
        }
    }
    return value;
}
} // namespace

void SongChooser::open(const std::vector<SongEntry> &songs, size_t previous) {
    songs_ = &songs;
    const bool found = previous > 0 && previous < songs.size();
    path_ = found ? containing_library(songs[previous]) : songs_root;
    refresh(found ? songs[previous].midi : "");
}

void SongChooser::refresh(const std::string &previous) {
    selected_ = 0;
    ++generation_;
    query_.clear();
    folded_.clear();
    items_.clear();
    indices_.clear();
    // Index zero is reserved by the original top-level Tutorial.
    items_.emplace_back();
    indices_.push_back(0);

    const std::string prefix = path_ + '/';
    std::map<std::string, int> folders;
    int sd_song_count = -1;
    for (const auto &song : *songs_) {
        if (song.tutorial) {
            continue;
        }
        const auto path = containing_library(song);
        if (path_ == songs_root && (path == sd_songs_root || path.compare(0, sizeof(sd_songs_root), "/sd/songs/") == 0)) {
            sd_song_count = std::max(0, sd_song_count) + (path == sd_songs_root);
        }
        if (path.compare(0, prefix.size(), prefix) != 0) {
            continue;
        }
        const auto slash = path.find('/', prefix.size());
        auto &count = folders[path.substr(0, slash)];
        count += slash == std::string::npos;
    }

    if (sd_song_count >= 0) {
        SongEntry directory;
        directory.title = "SD Card";
        directory.midi = sd_songs_root;
        directory.library_count = sd_song_count;
        items_.push_back(std::move(directory));
        indices_.push_back(0);
    }
    for (const auto &[path, count] : folders) {
        SongEntry directory;
        directory.library_count = count;
        read_folder(path, directory);
        items_.push_back(std::move(directory));
        indices_.push_back(0);
    }
    for (size_t i = 1; i < songs_->size(); ++i) {
        const auto &song = (*songs_)[i];
        if (containing_library(song) == path_) {
            items_.push_back(song);
            indices_.push_back(i);
        }
    }
    if (items_.size() > 1) {
        const auto found = std::find_if(items_.begin() + 1, items_.end(),
            [&](const SongEntry &item) { return item.midi == previous; });
        selected_ = found == items_.end() ? 1 : found - items_.begin();
    }
}

bool SongChooser::matches(const SongEntry &item) const {
    return query_.empty() || fold(item.title).find(folded_) != std::string::npos ||
           (item.library_count < 0 && fold(item.artist).find(folded_) != std::string::npos);
}

bool SongChooser::matches() const {
    return selected_ && matches(items_[selected_]);
}

void SongChooser::move(int step) {
    if (!matches()) {
        return;
    }
    const bool next = step > 0;
    for (int moves = std::abs(step); moves > 0; --moves) {
        do {
            if (next) {
                selected_ = selected_ + 1 < items_.size() ? selected_ + 1 : 1;
            } else {
                selected_ = selected_ > 1 ? selected_ - 1 : items_.size() - 1;
            }
        } while (!matches());
    }
}

void SongChooser::type(unsigned char symbol) {
    if (symbol == '\b') {
        if (!query_.empty()) {
            while ((static_cast<unsigned char>(query_.back()) & 0xc0) == 0x80) {
                query_.pop_back();
            }
            query_.pop_back();
        }
        folded_ = fold(query_);
        return;
    }
    if (symbol < 32 || (symbol >= 127 && symbol < 160) || query_.size() >= 70) {
        return;
    }
    if (symbol >= 160) {
        query_ += static_cast<char>(0xc0 | (symbol >> 6));
        symbol = 0x80 | (symbol & 0x3f);
    }
    query_ += static_cast<char>(symbol);
    folded_ = fold(query_);
    const auto found = std::find_if(items_.begin() + 1, items_.end(),
        [&](const SongEntry &item) { return matches(item); });
    if (found != items_.end()) {
        selected_ = found - items_.begin();
    }
}

bool SongChooser::enter() {
    if (!matches() || !folder()) {
        return false;
    }
    path_ = items_[selected_].midi;
    refresh("");
    return true;
}

bool SongChooser::parent() {
    if (path_ == songs_root) {
        return false;
    }
    const auto previous = path_;
    path_ = path_ == sd_songs_root ? songs_root : parent_path(path_);
    refresh(previous);
    return true;
}

} // namespace dc
