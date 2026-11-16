// SPDX-License-Identifier: GPL-2.0-or-later
#include "language.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace dc {
namespace {
constexpr const char *names[] = {
    "English", "Brazilian portuguese", "Czech", "Dutch", "Finnish", "French", "Galician",
    "German", "Hebrew", "Hungarian", "Italian", "Polish", "Russian", "Spanish", "Swedish", "Turkish"
};
}

unsigned Language::count() { return sizeof(names) / sizeof(names[0]); }

const char *Language::name(unsigned index) {
    return index < count() ? names[index] : names[0];
}

void Language::select(unsigned index) {
    if (index == selected_) {
        return;
    }
    selected_ = index;
    messages_.clear();
    catalog_.clear();
    reversed_ = false;
    if (index == 0 || index >= count()) {
        return;
    }

    char path[48];
    std::snprintf(path, sizeof(path), "/rd/lang/%u.cat", index);
    auto *file = std::fopen(path, "rb");
    if (!file) {
        return;
    }
    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    if (size > 0 && size <= 128 * 1024) {
        std::rewind(file);
        catalog_.resize(size);
        if (std::fread(catalog_.data(), 1, size, file) != size_t(size)) {
            catalog_.clear();
        }
    }
    std::fclose(file);

    for (size_t cursor = 0; cursor < catalog_.size();) {
        const auto take = [&]() -> const char * {
            const char *start = catalog_.data() + cursor;
            const auto *end = static_cast<const char *>(
                std::memchr(start, '\0', catalog_.size() - cursor));
            if (!end) {
                return nullptr;
            }
            cursor += end - start + 1;
            return start;
        };
        const auto *key = take();
        const auto *value = key && cursor < catalog_.size() ? take() : nullptr;
        if (!value || !*key || (!messages_.empty() && std::strcmp(messages_.back().first, key) >= 0)) {
            messages_.clear();
            break;
        }
        messages_.emplace_back(key, value);
    }
    reversed_ = std::strcmp(translate("__lefttoright__"), "__righttoleft__") == 0;
}

const char *Language::translate(const char *source) const {
    const auto message = std::lower_bound(messages_.begin(), messages_.end(), source,
        [](const auto &entry, const char *name) { return std::strcmp(entry.first, name) < 0; });
    return message != messages_.end() && std::strcmp(message->first, source) == 0 ? message->second : source;
}

std::string Language::format(const char *source, const std::string &value) const {
    std::string result;
    for (const char *p = translate(source); *p; ++p) {
        if (*p == '%' && (p[1] == 'd' || p[1] == 's')) {
            result += value;
            ++p;
        } else {
            result += *p;
            if (*p == '%' && p[1] == '%') {
                ++p;
            }
        }
    }
    return result;
}

} // namespace dc
