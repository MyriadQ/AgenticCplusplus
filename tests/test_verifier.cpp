#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "agenticpp/verifier.hpp"

TEST_CASE("Verifier: always-ok double passes") {
    agenticpp::AlwaysOkVerifier verifier;

    const auto verdict = verifier.verify(0);

    REQUIRE(verdict.ok);
    REQUIRE(verdict.checks.size() == 1);
    REQUIRE(verdict.checks.front().ok);
}

TEST_CASE("Verifier: health probe succeeds on HTTP 200") {
    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listen_fd >= 0);

    int reuse = 1;
    (void)::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    REQUIRE(::bind(listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(listen_fd, 1) == 0);

    socklen_t address_size = sizeof(address);
    REQUIRE(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&address), &address_size) == 0);
    const int port = static_cast<int>(ntohs(address.sin_port));

    std::jthread server([listen_fd](std::stop_token) {
        pollfd ready{listen_fd, POLLIN, 0};
        if (::poll(&ready, 1, 2000) > 0) {
            const int client = ::accept(listen_fd, nullptr, nullptr);
            if (client >= 0) {
                const char response[] =
                    "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                (void)::write(client, response, sizeof(response) - 1);
                ::close(client);
            }
        }
        ::close(listen_fd);
    });

    agenticpp::VerifierConfig config;
    config.health_url = "http://127.0.0.1:" + std::to_string(port) + "/health";
    config.health_timeout = std::chrono::milliseconds{1000};

    agenticpp::HealthProbeVerifier verifier(config);
    const auto verdict = verifier.verify(0);

    REQUIRE(verdict.ok);
    REQUIRE_FALSE(verdict.checks.empty());
    REQUIRE(verdict.checks.front().ok);
}

TEST_CASE("Verifier: health probe fails on connection refusal") {
    agenticpp::VerifierConfig config;
    config.health_url = "http://127.0.0.1:1/health";
    config.health_timeout = std::chrono::milliseconds{100};

    agenticpp::HealthProbeVerifier verifier(config);
    const auto verdict = verifier.verify(0);

    REQUIRE_FALSE(verdict.ok);
    REQUIRE_FALSE(verdict.checks.empty());
    REQUIRE_FALSE(verdict.checks.front().ok);
}

TEST_CASE("Verifier: invalid health URL is rejected") {
    agenticpp::VerifierConfig config;
    config.health_url = "not-a-url";
    config.health_timeout = std::chrono::milliseconds{100};

    agenticpp::HealthProbeVerifier verifier(config);
    const auto verdict = verifier.verify(0);

    REQUIRE_FALSE(verdict.ok);
    REQUIRE(verdict.checks.size() == 1);
    REQUIRE(verdict.checks.front().detail == "invalid health URL");
}
