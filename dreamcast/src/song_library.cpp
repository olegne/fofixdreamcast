// SPDX-License-Identifier: GPL-2.0-or-later
#include "song_library.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>
#include <utility>

namespace dc {
namespace {

constexpr size_t max_songs = 512;
constexpr unsigned max_directory_depth = 8;

bool is_file(const std::string &path) {
    struct stat info{};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

std::string trim(const std::string &value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

void read_ini(const std::string &path, SongEntry &song) {
    auto *file = std::fopen(path.c_str(), "r");
    if (!file) {
        return;
    }
    bool in_song = false;
    char buffer[512];
    while (std::fgets(buffer, sizeof(buffer), file)) {
        std::string line = trim(buffer);
        if (line.empty() || line.front() == '#' || line.front() == ';') {
            continue;
        }
        if (line.front() == '[') {
            in_song = line == "[song]";
            continue;
        }
        const auto equals = line.find('=');
        if (!in_song || equals == std::string::npos) {
            continue;
        }
        const auto key = trim(line.substr(0, equals));
        const auto value = trim(line.substr(equals + 1));
        if (key == "name" && !value.empty()) {
            song.title = value;
        } else if (key == "artist") {
            song.artist = value;
        } else if (key == "delay") {
            char *end;
            const auto delay = std::strtol(value.c_str(), &end, 10);
            if (*end == '\0') {
                song.delay_ms = static_cast<int>(std::clamp(delay, -10000L, 10000L));
            }
        }
    }
    std::fclose(file);
}

void load_directory(const std::string &path, std::vector<SongEntry> &songs, unsigned depth) {
    if (depth > max_directory_depth || songs.size() == max_songs) {
        return;
    }
    if (is_file(path + "/notes.mid")) {
        SongEntry song;
        song.title = path.substr(path.find_last_of('/') + 1);
        song.midi = path + "/notes.mid";
        const auto backing = path + "/song.ogg";
        const auto guitar = path + "/guitar.ogg";
        if (is_file(backing)) {
            song.backing = backing;
            if (is_file(guitar)) {
                song.guitar = guitar;
            }
        } else if (is_file(guitar)) {
            song.backing = guitar;
        } else {
            return;
        }
        read_ini(path + "/song.ini", song);
        if (path == "/cd/songs/tutorial") {
            return;  // The original opens Tutorial directly from the main menu.
        }
        songs.push_back(std::move(song));
        return;
    }

    auto *dir = opendir(path.c_str());
    if (!dir) {
        return;
    }
    while (auto *entry = readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        const auto child = path + "/" + entry->d_name;
        struct stat info{};
        if (stat(child.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
            load_directory(child, songs, depth + 1);
        }
        if (songs.size() == max_songs) {
            break;
        }
    }
    closedir(dir);
}

} // namespace

std::vector<SongEntry> song_library() {
    std::vector<SongEntry> songs;
    songs.push_back({"Tutorial", "Jurgen", "/rd/tutorial/notes.mid", "/rd/tutorial/song.ogg",
                     "/rd/tutorial/guitar.ogg", 0, true});
    const auto built_in = songs.size();
    load_directory("/cd/songs", songs, 0);
    std::sort(songs.begin() + built_in, songs.end(),
              [](const SongEntry &a, const SongEntry &b) { return a.title < b.title; });
    return songs;
}

} // namespace dc
