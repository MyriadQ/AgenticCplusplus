#include "agenticpp/verifier.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "agenticpp/line_buffer.hpp"
#include "agenticpp/template_parser.hpp"
#include "agenticpp/unique_fd.hpp"

namespace agenticpp {
namespace {

using Clock = std::chrono::steady_clock;

struct UrlParts {
    std::string host;
    std::string port;
    std::string path;
};

struct ProbeResult {
    bool ok = false;
    std::string detail;
};

bool parse_url(std::string_view url, UrlParts& parts) {
    constexpr std::string_view kScheme = "http://";
    if (!url.starts_with(kScheme)) {
        return false;
    }

    const std::string_view rest = url.substr(kScheme.size());
    const std::size_t slash = rest.find('/');
    const std::string_view authority = rest.substr(0, slash);
    parts.path = slash == std::string_view::npos ? "/" : std::string{rest.substr(slash)};

    const std::size_t colon = authority.rfind(':');
    if (colon == std::string_view::npos) {
        parts.host = std::string{authority};
        parts.port = "80";
    } else {
        parts.host = std::string{authority.substr(0, colon)};
        parts.port = std::string{authority.substr(colon + 1)};
    }
    return !parts.host.empty() && !parts.port.empty();
}

int timeout_ms(std::chrono::milliseconds timeout) {
    return static_cast<int>(std::min<std::int64_t>(timeout.count(), 60000));
}

ProbeResult probe_once(const UrlParts& parts, std::chrono::milliseconds timeout) {
    ProbeResult result;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* raw = nullptr;
    const int lookup = ::getaddrinfo(parts.host.c_str(), parts.port.c_str(), &hints, &raw);
    if (lookup != 0) {
        result.detail = std::string{"getaddrinfo: "} + ::gai_strerror(lookup);
        return result;
    }
    const std::unique_ptr<addrinfo, void (*)(addrinfo*)> addresses(raw, ::freeaddrinfo);

    for (addrinfo* address = addresses.get(); address != nullptr; address = address->ai_next) {
        UniqueFd fd{::socket(address->ai_family, address->ai_socktype, address->ai_protocol)};
        if (!fd) {
            continue;
        }

        const int flags = ::fcntl(fd.get(), F_GETFL, 0);
        if (flags >= 0) {
            (void)::fcntl(fd.get(), F_SETFL, flags | O_NONBLOCK);
        }

        int connected = ::connect(fd.get(), address->ai_addr, address->ai_addrlen);
        if (connected < 0 && errno != EINPROGRESS) {
            continue;
        }

        if (connected < 0) {
            pollfd poll_fd{fd.get(), POLLOUT, 0};
            const int ready = ::poll(&poll_fd, 1, timeout_ms(timeout));
            if (ready <= 0) {
                continue;
            }

            int error = 0;
            socklen_t length = sizeof(error);
            if (::getsockopt(fd.get(), SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) {
                continue;
            }
        }

        if (flags >= 0) {
            (void)::fcntl(fd.get(), F_SETFL, flags);
        }

        timeval socket_timeout{};
        socket_timeout.tv_sec = static_cast<time_t>(timeout.count() / 1000);
        socket_timeout.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
        (void)::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &socket_timeout,
                           sizeof(socket_timeout));
        (void)::setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO, &socket_timeout,
                           sizeof(socket_timeout));

        const std::string request = "GET " + parts.path + " HTTP/1.1\r\nHost: " + parts.host +
                                    ":" + parts.port + "\r\nConnection: close\r\n\r\n";

        std::size_t sent = 0;
        while (sent < request.size()) {
            const ssize_t count =
                ::send(fd.get(), request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
            if (count > 0) {
                sent += static_cast<std::size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            result.detail = std::string{"send: "} + std::strerror(errno);
            return result;
        }

        std::string response;
        char buffer[1024];
        while (response.find("\r\n") == std::string::npos) {
            const ssize_t count = ::recv(fd.get(), buffer, sizeof(buffer), 0);
            if (count > 0) {
                response.append(buffer, static_cast<std::size_t>(count));
                continue;
            }
            if (count == 0) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            result.detail = std::string{"recv: "} + std::strerror(errno);
            return result;
        }

        const std::size_t line_end = response.find("\r\n");
        if (line_end == std::string::npos) {
            result.detail = "no HTTP status line";
            return result;
        }

        const std::string_view status{response.data(), line_end};
        const std::size_t first_space = status.find(' ');
        if (first_space == std::string_view::npos || first_space + 4 > status.size()) {
            result.detail = "malformed HTTP status line";
            return result;
        }

        const std::string_view code = status.substr(first_space + 1, 3);
        result.detail = "HTTP " + std::string{code};
        result.ok = code == "200";
        return result;
    }

    result.detail = "connection failed";
    return result;
}

}  // namespace

Verdict AlwaysOkVerifier::verify(std::uint64_t) {
    Verdict verdict;
    verdict.ok = true;
    verdict.checks.push_back(Check{"always_ok", true, "test double"});
    return verdict;
}

HealthProbeVerifier::HealthProbeVerifier(VerifierConfig config) : config_(std::move(config)) {}

Verdict HealthProbeVerifier::verify(std::uint64_t) {
    Verdict verdict;

    UrlParts parts;
    if (!parse_url(config_.health_url, parts)) {
        verdict.checks.push_back(Check{"health", false, "invalid health URL"});
        return verdict;
    }

    const Clock::time_point deadline = Clock::now() + config_.health_timeout;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
        if (remaining.count() <= 0) {
            break;
        }

        ProbeResult probe = probe_once(parts, remaining);
        verdict.checks.push_back(Check{"health", probe.ok, probe.detail});
        if (probe.ok) {
            verdict.ok = true;
            return verdict;
        }

        if (attempt < 2) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - Clock::now());
            if (left.count() <= 0) {
                break;
            }
            std::this_thread::sleep_for(std::min(std::chrono::milliseconds{100}, left));
        }
    }

    return verdict;
}

LogQuietVerifier::LogQuietVerifier(std::unique_ptr<Verifier> inner,
                                   std::filesystem::path log_path,
                                   std::chrono::milliseconds quiet_window)
    : inner_(std::move(inner)),
      log_path_(std::move(log_path)),
      quiet_window_(quiet_window) {}

void LogQuietVerifier::before_repair(std::uint64_t) {
    armed_ = false;
    offset_ = 0;
    inode_ = 0;
    armed_at_ = {};
    if (log_path_.empty()) {
        return;
    }

    struct stat status {};
    if (::stat(log_path_.c_str(), &status) != 0) {
        return;
    }
    offset_ = static_cast<std::uint64_t>(status.st_size);
    inode_ = static_cast<std::uint64_t>(status.st_ino);
    armed_at_ = std::chrono::system_clock::now();
    armed_ = true;
}

Verdict LogQuietVerifier::verify(std::uint64_t template_id) {
    Verdict verdict = inner_->verify(template_id);
    if (log_path_.empty()) {
        return verdict;
    }
    if (!armed_) {
        verdict.ok = false;
        verdict.checks.push_back(
            Check{"log_quiet", false, "no pre-repair log snapshot"});
        return verdict;
    }

    const Timestamp deadline = armed_at_ + quiet_window_;
    const Timestamp current = std::chrono::system_clock::now();
    if (current < deadline) {
        std::this_thread::sleep_for(deadline - current);
    }

    struct stat status {};
    if (::stat(log_path_.c_str(), &status) != 0) {
        verdict.ok = false;
        verdict.checks.push_back(Check{"log_quiet", false, "cannot stat log file"});
        return verdict;
    }

    std::uint64_t start = offset_;
    if (static_cast<std::uint64_t>(status.st_ino) != inode_ ||
        static_cast<std::uint64_t>(status.st_size) < offset_) {
        // Rotation or truncation: inspect the replacement file from the start.
        start = 0;
    }

    constexpr std::uint64_t kMaxRead = 8ull * 1024 * 1024;
    std::uint64_t available =
        static_cast<std::uint64_t>(status.st_size) > start
            ? static_cast<std::uint64_t>(status.st_size) - start
            : 0;
    available = std::min(available, kMaxRead);

    std::string content;
    if (available > 0) {
        std::ifstream input(log_path_, std::ios::binary);
        if (!input) {
            verdict.ok = false;
            verdict.checks.push_back(Check{"log_quiet", false, "cannot open log file"});
            return verdict;
        }
        input.seekg(static_cast<std::streamoff>(start));
        content.resize(static_cast<std::size_t>(available));
        input.read(content.data(), static_cast<std::streamsize>(content.size()));
        content.resize(static_cast<std::size_t>(input.gcount()));
    }

    LineBuffer buffer;
    buffer.append(std::span<const char>(content.data(), content.size()));
    buffer.flush_eof();

    std::size_t matches = 0;
    while (const auto line = buffer.next_line()) {
        if (parse_template(*line).template_id == template_id) {
            ++matches;
        }
    }

    const bool quiet = matches == 0;
    std::string detail = quiet ? "incident template did not reappear"
                               : "incident template appeared " + std::to_string(matches) +
                                     " time(s) during the quiet window";
    verdict.checks.push_back(Check{"log_quiet", quiet, std::move(detail)});
    verdict.ok = verdict.ok && quiet;
    return verdict;
}

}  // namespace agenticpp
