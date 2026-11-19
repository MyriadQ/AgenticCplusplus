#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "agenticpp/executor.hpp"

namespace {

namespace fs = std::filesystem;

class TempDir {
public:
    explicit TempDir(std::string name) {
        path_ = fs::temp_directory_path() / ("agenticpp_sandbox_" + std::move(name));
        std::error_code error;
        fs::remove_all(path_, error);
        fs::create_directories(path_ / "data", error);
        fs::create_directories(path_ / "run", error);
        std::ofstream(path_ / "README") << "host content\n";
    }

    ~TempDir() {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

agenticpp::ExecutorConfig make_config(const fs::path& root, int tier, bool network = false) {
    agenticpp::ExecutorConfig config;
    config.timeout = std::chrono::seconds{5};
    config.grace = std::chrono::milliseconds{50};
    config.output_cap = 64 * 1024;
    config.env_allowlist = {"PATH"};
    config.limits.cpu_seconds = 10;
    config.limits.address_space = 512ull << 20;
    config.limits.nproc = 4096;
    config.limits.fsize = 1ull << 20;
    config.sandbox_tier = tier;
    config.sandbox_network = network;
    config.sandbox_root = root;
    config.sandbox_writable_paths = {"data", "run"};
    return config;
}

bool sandbox_unavailable(const agenticpp::ExecResult& result) {
    return result.exit_code == 126;
}

}  // namespace

TEST_CASE("Sandbox: chroot hides host root but keeps data writable") {
    TempDir temp("chroot");
    agenticpp::ChildProcessRunner runner(make_config(temp.path(), 1));

    const fs::path host_marker = fs::temp_directory_path() / "agenticpp_sandbox_marker";
    std::error_code error;
    fs::remove(host_marker, error);

    const std::string command = "echo inside > /tmp/agenticpp_sandbox_marker; "
                                "echo data > '" +
                                (temp.path() / "data" / "written").string() + "'";
    const auto result = runner.run({"/bin/sh", "-c", command});
    if (sandbox_unavailable(result)) {
        WARN(result.err);
        SKIP("user/mount namespaces are unavailable in this environment");
    }

    REQUIRE(result.exit_code == 0);
    REQUIRE_FALSE(fs::exists(host_marker));
    REQUIRE(fs::exists(temp.path() / "data" / "written"));
    REQUIRE_FALSE(fs::exists(temp.path() / "agenticpp_sandbox_marker"));
    fs::remove(host_marker, error);
}

TEST_CASE("Sandbox: project tree is read-only inside the namespaces") {
    TempDir temp("readonly");
    agenticpp::ChildProcessRunner runner(make_config(temp.path(), 1));

    const std::string command = "echo nope > '" + (temp.path() / "README").string() + "'";
    const auto result = runner.run({"/bin/sh", "-c", command});
    if (sandbox_unavailable(result)) {
        SKIP("user/mount namespaces are unavailable in this environment");
    }

    REQUIRE(result.exit_code != 0);
}

TEST_CASE("Sandbox: network namespace exposes only loopback") {
    TempDir temp("network");
    const fs::path probe = temp.path() / "sandbox_probe";
    std::error_code copy_error;
    fs::copy_file(AGENTICPP_SANDBOX_PROBE, probe, fs::copy_options::overwrite_existing,
                  copy_error);
    REQUIRE_FALSE(copy_error);
    fs::permissions(probe, fs::perms::owner_exec | fs::perms::owner_read |
                               fs::perms::owner_write,
                    fs::perm_options::add, copy_error);
    REQUIRE_FALSE(copy_error);

    agenticpp::ChildProcessRunner runner(make_config(temp.path(), 1, true));
    const auto result = runner.run({probe.string(), "interfaces"});
    if (sandbox_unavailable(result)) {
        SKIP("user/mount namespaces are unavailable in this environment");
    }

    REQUIRE(result.exit_code == 0);
    REQUIRE(result.out.find("iface=lo") != std::string::npos);
    REQUIRE(result.out.find("enp") == std::string::npos);
    REQUIRE(result.out.find("eth") == std::string::npos);
    REQUIRE(result.out.find("wlan") == std::string::npos);
    REQUIRE(result.out.find("tailscale") == std::string::npos);
}

TEST_CASE("Sandbox: seccomp denies socket while the shell still runs") {
    TempDir temp("seccomp");
    const fs::path probe = temp.path() / "sandbox_probe";
    std::error_code copy_error;
    fs::copy_file(AGENTICPP_SANDBOX_PROBE, probe, fs::copy_options::overwrite_existing,
                  copy_error);
    REQUIRE_FALSE(copy_error);
    fs::permissions(probe, fs::perms::owner_exec | fs::perms::owner_read |
                               fs::perms::owner_write,
                    fs::perm_options::add, copy_error);
    REQUIRE_FALSE(copy_error);

    agenticpp::ChildProcessRunner runner(make_config(temp.path(), 2));

    const auto result = runner.run({probe.string(), "socket"});
    if (sandbox_unavailable(result)) {
        SKIP("seccomp support is unavailable in this build");
    }

    REQUIRE(result.exit_code == 0);
    REQUIRE(result.out.find("socket errno=1") != std::string::npos);

    const auto shell = runner.run({"/bin/sh", "-c", "echo seccomp-ok"});
    REQUIRE(shell.exit_code == 0);
    REQUIRE(shell.out.find("seccomp-ok") != std::string::npos);
}
