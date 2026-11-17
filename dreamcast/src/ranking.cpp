// SPDX-License-Identifier: GPL-2.0-or-later
#include "ranking.hpp"

#include <arch/timer.h>
#include <cerrno>
#include <cstdio>
#include <dc/net/broadband_adapter.h>
#include <dc/net/lan_adapter.h>
#include <fcntl.h>
#include <kos/fs.h>
#include <kos/net.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dc {
namespace {

constexpr char classic_endpoint[] = "http://fretsonfire.sourceforge.net/play";
constexpr unsigned socket_timeout_ms = 8000;
constexpr size_t response_limit = 8192;
constexpr size_t body_limit = 512;

std::string configured_url() {
    auto *file = std::fopen("/cd/ranking.url", "rb");
    if (!file) {
        return errno == ENOENT ? classic_endpoint : std::string{};
    }
    char bytes[513];
    const size_t count = std::fread(bytes, 1, sizeof(bytes), file);
    const bool failed = std::ferror(file);
    std::fclose(file);
    // Make an invalid override fail explicitly. Falling back would send to another server.
    return failed || count == sizeof(bytes) ? std::string{} : std::string(bytes, count);
}

bool head_is(const std::string_view line, const std::string_view field) {
    if (line.size() < field.size()) {
        return false;
    }
    for (size_t i = 0; i < field.size(); ++i) {
        const char c = line[i];
        if (field[i] != (c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c)) {
            return false;
        }
    }
    return true;
}

bool parse_headers(const std::string_view headers, size_t &length) {
    const auto line_end = headers.find("\r\n");
    const auto first = headers.substr(0, line_end);
    const auto status = first.find(' ');
    if (!head_is(first, "http/1.") || status == std::string_view::npos || status + 4 > first.size() ||
        first[status + 1] != '2' || first[status + 2] < '0' || first[status + 2] > '9' ||
        first[status + 3] < '0' || first[status + 3] > '9' ||
        (status + 4 < first.size() && first[status + 4] != ' ')) {
        return false;
    }
    length = std::string_view::npos;
    if (line_end == std::string_view::npos) {
        return true;
    }
    auto fields = headers.substr(line_end + 2);
    while (!fields.empty()) {
        const auto next = fields.find("\r\n");
        const auto field = fields.substr(0, next);
        constexpr std::string_view content_length = "content-length:";
        if (head_is(field, content_length)) {
            auto number = field.substr(content_length.size());
            while (!number.empty() && (number.front() == ' ' || number.front() == '\t')) {
                number.remove_prefix(1);
            }
            while (!number.empty() && (number.back() == ' ' || number.back() == '\t')) {
                number.remove_suffix(1);
            }
            if (number.empty() || number.size() > 9) {
                return false;
            }
            size_t count = 0;
            for (char c : number) {
                if (c < '0' || c > '9') {
                    return false;
                }
                count = count * 10 + size_t(c - '0');
            }
            if (count > body_limit || (length != std::string_view::npos && length != count)) {
                return false;
            }
            length = count;
        } else if (head_is(field, "transfer-encoding:")) {
            return false;
        }
        if (next == std::string_view::npos) {
            break;
        }
        fields.remove_prefix(next + 2);
    }
    return true;
}

ranking::Reply exchange(int socket_fd, const sockaddr *host, socklen_t length, const std::string &request) {
    if (fs_fcntl(socket_fd, F_SETFL, O_NONBLOCK) < 0 ||
        (connect(socket_fd, host, length) < 0 && errno != EINPROGRESS)) {
        return {};
    }

    const auto deadline = timer_ms_gettime64() + socket_timeout_ms;
    size_t sent = 0;
    while (sent != request.size()) {
        if (timer_ms_gettime64() >= deadline) {
            return {};
        }
        const auto count = send(socket_fd, request.data() + sent, request.size() - sent, 0);
        if (count > 0) {
            sent += count;
        } else if (count < 0 && (errno == ENOTCONN || errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            thd_sleep(10);
        } else {
            return {};
        }
    }

    std::string response;
    auto body_at = std::string::npos;
    auto content_length = std::string::npos;
    char bytes[512];
    while (timer_ms_gettime64() < deadline) {
        const auto count = recv(socket_fd, bytes, sizeof(bytes), 0);
        if (count == 0) {
            if (body_at != std::string::npos && content_length == std::string::npos) {
                return ranking::parse_reply(std::string_view(response).substr(body_at));
            }
            return {};
        }
        if (count < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                return {};
            }
            thd_sleep(10);
            continue;
        }
        response.append(bytes, count);
        if (response.size() > response_limit) {
            return {};
        }

        if (body_at == std::string::npos) {
            const auto marker = response.find("\r\n\r\n");
            if (marker == std::string::npos) {
                continue;
            }
            if (!parse_headers(std::string_view(response).substr(0, marker), content_length)) {
                return {};
            }
            body_at = marker + 4;
        }
        const auto body = std::string_view(response).substr(body_at);
        if (content_length != std::string::npos && body.size() >= content_length) {
            return ranking::parse_reply(body.substr(0, content_length));
        }
        if (body.size() > body_limit) {
            return {};
        }
    }
    return {};
}

bool bind_upload_socket(int socket_fd) {
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(net_ipv4_address(net_default_dev->ip_addr));
    // KOS's implicit bind can reuse port 1024 even while the last request is in TIME_WAIT.
    static unsigned next_port = timer_ms_gettime64() & 0x3fff;
    for (unsigned i = 0; i < 8; ++i) {
        local.sin_port = htons(49152 + (next_port++ & 0x3fff));
        if (bind(socket_fd, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) == 0) {
            return true;
        }
        if (errno != EADDRINUSE) {
            return false;
        }
    }
    return false;
}

ranking::Reply submit(const ranking::Endpoint &endpoint, const std::string &request) {
    // INIT_DEFAULT omits the network drivers. Register them at the first opt-in upload.
    static bool registered = false;
    if (!registered) {
        if (!net_default_dev) {
            bba_init();
            la_init();
        }
        registered = true;
    }
    if (net_init(0) < 0 || !net_default_dev || net_default_dev->ip_addr[0] == 0) {
        return {};
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *resolved = nullptr;
    if (getaddrinfo(endpoint.host.c_str(), endpoint.port.c_str(), &hints, &resolved) != 0 || !resolved) {
        return {};
    }
    const auto socket_fd = socket(resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
    ranking::Reply reply;
    if (socket_fd >= 0) {
        if (bind_upload_socket(socket_fd)) {
            reply = exchange(socket_fd, resolved->ai_addr, resolved->ai_addrlen, request);
        }
        close(socket_fd);
    }
    freeaddrinfo(resolved);
    return reply;
}

} // namespace

ScoreUpload::~ScoreUpload() {
    if (thread_) {
        thd_join(thread_, nullptr);
    }
}

void ScoreUpload::start(const ScoreBook &scores, const std::string &midi_path, const std::vector<uint8_t> *edited) {
    poll();
    if (thread_) {
        status_ = UploadStatus::failed;
        return;
    }
    ordinal_ = 0;
    job_.midi_path = midi_path;
    job_.edited_midi = edited ? *edited : std::vector<uint8_t>{};
    for (unsigned difficulty = 0; difficulty < job_.tables.size(); ++difficulty) {
        job_.tables[difficulty] = scores.table(midi_path, difficulty);
    }
    job_.reply = {};
    done_.store(false, std::memory_order_relaxed);
    thread_ = thd_create(0, send_scores, this);
    status_ = thread_ ? UploadStatus::pending : UploadStatus::failed;
    if (thread_) {
        std::puts("FoFDC: score upload pending");
    }
}

void ScoreUpload::poll() {
    if (!thread_ || !done_.load(std::memory_order_acquire)) {
        return;
    }
    thd_join(thread_, nullptr);
    thread_ = nullptr;
    if (status_ == UploadStatus::pending) {
        status_ = job_.reply.success ? UploadStatus::succeeded : UploadStatus::failed;
        ordinal_ = job_.reply.ordinal;
        std::printf("FoFDC: score upload %s ordinal=%d\n", job_.reply.success ? "succeeded" : "failed", ordinal_);
    }
    job_ = {};
}

void ScoreUpload::reset() {
    status_ = UploadStatus::idle;
    ordinal_ = 0;
}

void *ScoreUpload::send_scores(void *context) {
    auto &upload = *static_cast<ScoreUpload *>(context);
    auto &job = upload.job_;
    ranking::Endpoint endpoint;
    if (ranking::parse_endpoint(configured_url(), endpoint)) {
        const auto note = job.midi_path.rfind('/');
        const auto directory = note == std::string::npos ? std::string{} : job.midi_path.substr(0, note);
        const auto name = directory.substr(directory.rfind('/') + 1);
        const auto hash = ranking::song_hash(job.midi_path, job.edited_midi);
        if (!name.empty() && !hash.empty()) {
            job.reply = submit(endpoint, ranking::request(endpoint, name, hash, job.tables));
        }
    }
    upload.done_.store(true, std::memory_order_release);
    return nullptr;
}

} // namespace dc
