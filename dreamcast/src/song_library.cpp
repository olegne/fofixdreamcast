// SPDX-License-Identifier: GPL-2.0-or-later
#include "song_library.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dc/scif.h>
#include <dc/sd.h>
#include <fat/fs_fat.h>
#include <kos/dbgio.h>
#include <sys/stat.h>
#include <utility>

namespace dc {
namespace {

constexpr size_t max_songs = 512;
constexpr unsigned max_directory_depth = 8;

bool supported_fat_partition(kos_blockdev_t &device, uint8_t type) {
    const bool fat32 = type == 0x0b || type == 0x0c;
    if (!fat32 && type != 0x04 && type != 0x06 && type != 0x0e) {
        return false;
    }
    alignas(32) uint8_t boot[512];
    if (device.read_blocks(&device, 0, 1, boot) != 0 || boot[510] != 0x55 || boot[511] != 0xaa) {
        return false;
    }
    const auto le16 = [&](unsigned i) { return uint32_t(boot[i]) | (uint32_t(boot[i + 1]) << 8); };
    const auto le32 = [&](unsigned i) { return le16(i) | (le16(i + 2) << 16); };
    const auto cluster_sectors = boot[13];
    const auto reserved = le16(14);
    const auto fats = boot[16];
    const auto root_entries = le16(17);
    const auto sectors16 = le16(19);
    const auto fat_size16 = le16(22);
    const auto sectors = sectors16 ? sectors16 : le32(32);
    const auto fat_size = fat32 ? le32(36) : fat_size16;
    const auto data_start = uint64_t(reserved) + fats * uint64_t(fat_size) + (root_entries + 15) / 16;
    // libkosfat computes cluster counts from the BPB before validating it.
    if (le16(11) != sizeof(boot) || !cluster_sectors || (cluster_sectors & (cluster_sectors - 1)) ||
        !reserved || !fats || fats > 2 || !fat_size || data_start >= sectors || sectors > device.count_blocks(&device)) {
        return false;
    }
    const auto clusters = (sectors - data_start) / cluster_sectors;
    if (fat32) {
        const auto root = le32(44);
        return !root_entries && !fat_size16 && clusters >= 65525 && root >= 2 && root < clusters + 2 &&
               le16(48) < reserved && (clusters + 2) * 4 <= uint64_t(fat_size) * sizeof(boot);
    }
    return root_entries && clusters >= 4085 && clusters < 65525 &&
           (clusters + 2) * 2 <= uint64_t(fat_size) * sizeof(boot);
}

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
        } else if (key == "cassettecolor" && value.size() == 7 && value.front() == '#') {
            char *end;
            const auto color = std::strtoul(value.c_str() + 1, &end, 16);
            if (*end == '\0' && color <= 0xffffff) {
                song.cassette_color = 0xff000000 | static_cast<uint32_t>(color);
            }
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

void load_directory(const std::string &path, std::vector<SongEntry> &songs, unsigned depth, bool imports) {
    if (depth > max_directory_depth || songs.size() == max_songs || path.size() + sizeof("/notes.mid") - 1 > 511) {
        return;
    }
    const bool has_chart = is_file(path + "/notes.mid");
    const auto guitar = path + "/guitar.ogg";
    if (has_chart || (imports && is_file(guitar))) {
        SongEntry song;
        song.title = path.substr(path.find_last_of('/') + 1);
        song.midi = path + "/notes.mid";
        song.blank_chart = !has_chart;
        const auto backing = path + "/song.ogg";
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
        const auto rhythm = path + "/rhythm.ogg";
        if (is_file(rhythm)) {
            song.rhythm = rhythm;
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
            load_directory(child, songs, depth + 1, imports);
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
                     "/rd/tutorial/guitar.ogg", {}, 0, true});
    const auto built_in = songs.size();
    load_directory("/cd/songs", songs, 0, false);
    std::sort(songs.begin() + built_in, songs.end(),
              [](const SongEntry &a, const SongEntry &b) { return a.title < b.title; });
    return songs;
}

std::vector<SongEntry> imported_song_sources() {
    std::vector<SongEntry> songs;
    load_directory("/cd/imports", songs, 0, true);
    std::sort(songs.begin(), songs.end(),
              [](const SongEntry &a, const SongEntry &b) { return a.title < b.title; });
    return songs;
}

const char *load_sd_song_library(std::vector<SongEntry> &songs, std::vector<SongEntry> &imports) {
    static kos_blockdev_t partition{};
    static bool mounted = false;
    if (mounted) {
        return "SD library already loaded. Restart the game to change cards.";
    }
    // The serial SD adapter and SCIF debug console use the same pins.
    const auto *debug_device = dbgio_dev_get();
    const bool serial_debug = debug_device && std::strcmp(debug_device, "scif") == 0;
    const auto restore_debug = [&] {
        if (serial_debug) {
            scif_init();
            dbgio_dev_select(debug_device);
        }
    };
    if (serial_debug) {
        dbgio_dev_select("null");
    }
    if (sd_init() != 0) {
        restore_debug();
        return "Could not initialize the serial SD adapter and card.";
    }
    fs_fat_init();
    uint8_t partition_type = 0;
    const bool has_partition = sd_blockdev_for_partition(0, &partition, &partition_type) == 0;
    const bool supported = has_partition && supported_fat_partition(partition, partition_type);
    if (!supported || fs_fat_mount("/sd", &partition, FS_FAT_MOUNT_READONLY) != 0) {
        if (has_partition && !supported) {
            partition.shutdown(&partition);
        }
        fs_fat_shutdown();
        sd_shutdown();
        restore_debug();
        return "The first SD partition must be FAT16 or FAT32 (MBR).";
    }
    load_directory(sd_songs_root, songs, 0, false);
    load_directory("/sd/imports", imports, 0, true);
    if (songs.empty() && imports.empty()) {
        fs_fat_unmount("/sd");
        fs_fat_shutdown();
        sd_shutdown();
        restore_debug();
        return "No songs or imports were found on the SD card.";
    }
    mounted = true;
    const auto by_title = [](const SongEntry &a, const SongEntry &b) { return a.title < b.title; };
    std::sort(songs.begin(), songs.end(), by_title);
    std::sort(imports.begin(), imports.end(), by_title);
    return nullptr;
}

} // namespace dc
