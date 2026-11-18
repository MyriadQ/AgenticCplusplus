// victim — small demo service: writes healthy log lines and serves GET /health.
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "agenticpp/unique_fd.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

extern "C" void handle_signal(int) {
    g_stop = 1;
}

void print_usage(std::string_view argv0) {
    std::printf(
        "usage: %.*s [--log PATH] [--port PORT]\n"
        "  --log PATH   append healthy/error lines (default data/logs/victim.log)\n"
        "  --port PORT  listen port (default 18080)\n",
        static_cast<int>(argv0.size()), argv0.data());
}

bool parse_port(std::string_view text, int& port) {
    if (text.empty()) {
        return false;
    }
    const std::string value_text{text};
    char* end = nullptr;
    const long value = std::strtol(value_text.c_str(), &end, 10);
    if (end == nullptr || *end != '\0' || value <= 0 || value > 65535) {
        return false;
    }
    port = static_cast<int>(value);
    return true;
}

std::string http_response(std::string_view status, std::string_view body) {
    return "HTTP/1.1 " + std::string{status} +
           "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\n\r\n" + std::string{body};
}

}  // namespace

int main(int argc, char** argv) {
    std::string log_path = "data/logs/victim.log";
    int port = 18080;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0] != nullptr ? argv[0] : "victim");
            return 0;
        }
        if (arg == "--log") {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "victim: --log requires a path\n");
                return 2;
            }
            log_path = argv[++i];
            continue;
        }
        if (arg == "--port") {
            if (i + 1 >= argc || !parse_port(argv[++i], port)) {
                std::fprintf(stderr, "victim: --port requires a valid port\n");
                return 2;
            }
            continue;
        }
        std::fprintf(stderr, "victim: unknown option '%.*s'\n", static_cast<int>(arg.size()),
                     arg.data());
        return 2;
    }

    const std::filesystem::path log_file{log_path};
    if (!log_file.parent_path().empty()) {
        std::error_code error;
        std::filesystem::create_directories(log_file.parent_path(), error);
    }

    std::ofstream log(log_path, std::ios::app);
    if (!log) {
        std::fprintf(stderr, "victim: cannot open log '%s'\n", log_path.c_str());
        return 2;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGPIPE, SIG_IGN);

    agenticpp::UniqueFd listen_fd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    if (!listen_fd) {
        std::fprintf(stderr, "victim: socket: %s\n", std::strerror(errno));
        return 2;
    }

    int reuse = 1;
    (void)::setsockopt(listen_fd.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<std::uint16_t>(port));

    if (::bind(listen_fd.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        log << "error service=victim bind: address already in use port=" << port << '\n';
        log.flush();
        std::fprintf(stderr, "victim: bind: address already in use on port %d\n", port);
        return 1;
    }
    if (::listen(listen_fd.get(), 8) != 0) {
        std::fprintf(stderr, "victim: listen: %s\n", std::strerror(errno));
        return 2;
    }

    std::printf("victim: listening on 127.0.0.1:%d log=%s\n", port, log_path.c_str());
    std::fflush(stdout);

    std::size_t tick = 0;
    log << "level=info service=victim tick=" << tick << " port=" << port << '\n';
    log.flush();

    while (g_stop == 0) {
        pollfd ready{listen_fd.get(), POLLIN, 0};
        const int result = ::poll(&ready, 1, 500);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::fprintf(stderr, "victim: poll: %s\n", std::strerror(errno));
            break;
        }

        if (result == 0) {
            ++tick;
            log << "level=info service=victim tick=" << tick << " port=" << port << '\n';
            log.flush();
            continue;
        }

        agenticpp::UniqueFd client{::accept(listen_fd.get(), nullptr, nullptr)};
        if (!client) {
            continue;
        }

        timeval timeout{};
        timeout.tv_sec = 1;
        (void)::setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        char request[2048];
        const ssize_t count = ::recv(client.get(), request, sizeof(request) - 1, 0);
        if (count <= 0) {
            continue;
        }
        request[count] = '\0';

        const std::string_view request_view{request, static_cast<std::size_t>(count)};
        const std::string response = request_view.starts_with("GET /health ")
                                         ? http_response("200 OK", R"({"ok":true})")
                                         : http_response("404 Not Found", R"({"ok":false})");
        (void)::send(client.get(), response.data(), response.size(), MSG_NOSIGNAL);
    }

    log << "level=info service=victim stopping\n";
    log.flush();
    return 0;
}
