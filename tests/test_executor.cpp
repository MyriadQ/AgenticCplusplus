#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include <vector>

#include "agenticpp/executor.hpp"

namespace {

agenticpp::ExecutorConfig make_config(std::chrono::milliseconds timeout = std::chrono::seconds{5}) {
    agenticpp::ExecutorConfig config;
    config.timeout = timeout;
    config.grace = std::chrono::milliseconds{50};
    config.output_cap = 64 * 1024;
    config.env_allowlist = {"PATH"};
    config.limits.cpu_seconds = 10;
    config.limits.address_space = 512ull << 20;
    config.limits.nproc = 4096;
    config.limits.fsize = 1ull << 20;
    return config;
}

}  // namespace

TEST_CASE("Executor: echo returns stdout and exit zero") {
    agenticpp::ChildProcessRunner runner(make_config());

    const auto result = runner.run({"/bin/echo", "hi"});

    REQUIRE(result.exit_code == 0);
    REQUIRE(result.term_signal == 0);
    REQUIRE(result.out == "hi\n");
    REQUIRE_FALSE(result.timed_out);
}

TEST_CASE("Executor: timeout kills the process group") {
    agenticpp::ChildProcessRunner runner(make_config(std::chrono::milliseconds{100}));

    const auto result = runner.run({"/bin/sh", "-c", "sleep 5"});

    REQUIRE(result.timed_out);
    REQUIRE(result.exit_code == -1);
    REQUIRE(result.elapsed < std::chrono::seconds{2});
}

TEST_CASE("Executor: nonexistent program returns 127") {
    agenticpp::ChildProcessRunner runner(make_config());

    const auto result = runner.run({"/nonexistent-program-for-agenticpp"});

    REQUIRE(result.exit_code == 127);
    REQUIRE_FALSE(result.timed_out);
}

TEST_CASE("Executor: output over cap is truncated") {
    auto config = make_config();
    config.output_cap = 4;

    agenticpp::ChildProcessRunner runner(config);
    const auto result = runner.run({"/bin/sh", "-c", "printf abcdefghij"});

    REQUIRE(result.exit_code == 0);
    REQUIRE(result.truncated);
    REQUIRE(result.out == "abcd");
}

TEST_CASE("Executor: child does not inherit DEEPSEEK_API_KEY") {
    ::setenv("DEEPSEEK_API_KEY", "secret", 1);

    auto config = make_config();
    config.env_allowlist = {"PATH"};

    agenticpp::ChildProcessRunner runner(config);
    const auto result = runner.run({"/bin/sh", "-c", "printf %s \"$DEEPSEEK_API_KEY\""});

    ::unsetenv("DEEPSEEK_API_KEY");

    REQUIRE(result.exit_code == 0);
    REQUIRE(result.out.empty());
}

TEST_CASE("Executor: CPU rlimit terminates a busy loop") {
    auto config = make_config(std::chrono::seconds{5});
    config.limits.cpu_seconds = 1;

    agenticpp::ChildProcessRunner runner(config);
    const auto result = runner.run({"/bin/sh", "-c", "while :; do :; done"});

    REQUIRE_FALSE(result.timed_out);
    REQUIRE(result.exit_code == -1);
    REQUIRE((result.term_signal == SIGXCPU || result.term_signal == SIGKILL));
}

TEST_CASE("Executor: no zombie remains after timeout") {
    agenticpp::ChildProcessRunner runner(make_config(std::chrono::milliseconds{100}));

    const auto result = runner.run({"/bin/sh", "-c", "sleep 5"});
    REQUIRE(result.timed_out);

    errno = 0;
    const pid_t waited = ::waitpid(-1, nullptr, WNOHANG);
    REQUIRE(waited == -1);
    REQUIRE(errno == ECHILD);
}
