#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "agenticpp/policy.hpp"

namespace {

std::filesystem::path source_root() {
    return std::filesystem::path{AGENTICPP_SOURCE_DIR};
}

agenticpp::Result<agenticpp::Policy> load_policy() {
    return agenticpp::Policy::from_file(source_root() / "config/agent.json", source_root());
}

class TempDir {
public:
    explicit TempDir(std::string name) {
        path_ = std::filesystem::temp_directory_path() / ("agenticpp_policy_" + name);
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        std::filesystem::create_directories(path_ / "scripts");
        std::filesystem::create_directories(path_ / "data");
    }

    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void write_file(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output(path);
    output << text;
}

void write_config(const std::filesystem::path& path, std::string_view json) {
    write_file(path, json);
}

}  // namespace

TEST_CASE("Policy: exact allowlisted argv is allowed") {
    auto result = load_policy();
    REQUIRE(result.has_value());

    const agenticpp::Policy& policy = result.value();
    const auto verdict = policy.check({"scripts/kill-decoy.sh"});

    REQUIRE(verdict.decision == agenticpp::Decision::Allow);
    REQUIRE(verdict.rule_id == "kill-decoy");
    REQUIRE(policy.rules().size() == 3);
}

TEST_CASE("Policy: shell command is denied") {
    auto result = load_policy();
    REQUIRE(result.has_value());

    const auto verdict = result.value().check({"sh", "-c", "rm -rf /"});

    REQUIRE(verdict.decision == agenticpp::Decision::Deny);
    REQUIRE(verdict.reason.find("deny list") != std::string::npos);
}

TEST_CASE("Policy: path traversal is denied") {
    auto result = load_policy();
    REQUIRE(result.has_value());

    const auto verdict = result.value().check({"scripts/../../etc/passwd"});

    REQUIRE(verdict.decision == agenticpp::Decision::Deny);
    REQUIRE(verdict.reason.find("traversal") != std::string::npos);
}

TEST_CASE("Policy: too many args is denied") {
    auto result = load_policy();
    REQUIRE(result.has_value());

    const auto verdict = result.value().check({"scripts/kill-decoy.sh", "extra"});

    REQUIRE(verdict.decision == agenticpp::Decision::Deny);
    REQUIRE(verdict.rule_id == "kill-decoy");
}

TEST_CASE("Policy: unknown program is denied") {
    auto result = load_policy();
    REQUIRE(result.has_value());

    const auto verdict = result.value().check({"scripts/unknown.sh"});

    REQUIRE(verdict.decision == agenticpp::Decision::Deny);
    REQUIRE(verdict.reason.find("not allowlisted") != std::string::npos);
}

TEST_CASE("Policy: empty argv is denied") {
    auto result = load_policy();
    REQUIRE(result.has_value());

    const auto verdict = result.value().check({});

    REQUIRE(verdict.decision == agenticpp::Decision::Deny);
    REQUIRE(verdict.reason.find("empty") != std::string::npos);
}

TEST_CASE("Policy: NUL byte is denied") {
    auto result = load_policy();
    REQUIRE(result.has_value());

    std::string program = "scripts/kill-decoy.sh";
    program.push_back('\0');

    const auto verdict = result.value().check({program});

    REQUIRE(verdict.decision == agenticpp::Decision::Deny);
    REQUIRE(verdict.reason.find("NUL") != std::string::npos);
}

TEST_CASE("Policy: program must use an explicit path") {
    auto result = load_policy();
    REQUIRE(result.has_value());

    const auto verdict = result.value().check({"kill-decoy.sh"});

    REQUIRE(verdict.decision == agenticpp::Decision::Deny);
    REQUIRE(verdict.reason.find("explicit path") != std::string::npos);
}

TEST_CASE("Policy: missing config returns an error") {
    auto result =
        agenticpp::Policy::from_file(source_root() / "config/does-not-exist.json", source_root());

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code != 0);
}

TEST_CASE("Policy: exact allowlisted argv after resolution is allowed") {
    TempDir temp("resolution");
    write_file(temp.path() / "scripts/kill-decoy.sh", "#!/bin/sh\n");
    write_config(temp.path() / "config.json",
                 R"({"allow":[{"id":"kill-decoy","program":"scripts/kill-decoy.sh","max_args":0}],"deny_programs":["sh"]})");

    auto result = agenticpp::Policy::from_file(temp.path() / "config.json", temp.path());
    REQUIRE(result.has_value());

    const auto verdict = result.value().check({"scripts/kill-decoy.sh"});

    REQUIRE(verdict.decision == agenticpp::Decision::Allow);
    REQUIRE(verdict.rule_id == "kill-decoy");
}

TEST_CASE("Policy: symlink to shell is denied") {
    TempDir temp("symlink");
    write_file(temp.path() / "scripts/restart-victim.sh", "#!/bin/sh\n");
    write_config(temp.path() / "config.json",
                 R"({"allow":[{"id":"restart-victim","program":"scripts/restart-victim.sh","max_args":0}],"deny_programs":["sh"]})");

    auto result = agenticpp::Policy::from_file(temp.path() / "config.json", temp.path());
    REQUIRE(result.has_value());

    std::error_code error;
    std::filesystem::remove(temp.path() / "scripts/restart-victim.sh", error);
    REQUIRE_FALSE(error);
    std::filesystem::create_symlink("/bin/sh", temp.path() / "scripts/restart-victim.sh", error);
    REQUIRE_FALSE(error);

    const auto verdict = result.value().check({"scripts/restart-victim.sh"});

    REQUIRE(verdict.decision == agenticpp::Decision::Deny);
    REQUIRE(verdict.reason.find("symlink") != std::string::npos);
}

TEST_CASE("Policy: absolute path outside root is denied") {
    TempDir temp("outside");
    write_config(temp.path() / "config.json",
                 R"({"allow":[{"id":"kill-decoy","program":"scripts/kill-decoy.sh","max_args":0}],"deny_programs":[]})");

    auto result = agenticpp::Policy::from_file(temp.path() / "config.json", temp.path());
    REQUIRE(result.has_value());

    const auto verdict = result.value().check({"/etc/passwd"});

    REQUIRE(verdict.decision == agenticpp::Decision::Deny);
    REQUIRE(verdict.reason.find("outside") != std::string::npos);
}

TEST_CASE("Policy: absolute argument outside data is denied") {
    TempDir temp("arg-outside");
    write_file(temp.path() / "scripts/kill-decoy.sh", "#!/bin/sh\n");
    write_config(temp.path() / "config.json",
                 R"({"allow":[{"id":"kill-decoy","program":"scripts/kill-decoy.sh","max_args":1}],"deny_programs":[]})");

    auto result = agenticpp::Policy::from_file(temp.path() / "config.json", temp.path());
    REQUIRE(result.has_value());

    const auto verdict = result.value().check({"scripts/kill-decoy.sh", "/etc/passwd"});

    REQUIRE(verdict.decision == agenticpp::Decision::Deny);
    REQUIRE(verdict.reason.find("data/") != std::string::npos);
}

TEST_CASE("Policy: absolute argument inside data is allowed") {
    TempDir temp("arg-inside");
    write_file(temp.path() / "scripts/kill-decoy.sh", "#!/bin/sh\n");
    write_file(temp.path() / "data/input.log", "sample\n");
    write_config(temp.path() / "config.json",
                 R"({"allow":[{"id":"kill-decoy","program":"scripts/kill-decoy.sh","max_args":1}],"deny_programs":[]})");

    auto result = agenticpp::Policy::from_file(temp.path() / "config.json", temp.path());
    REQUIRE(result.has_value());

    const auto verdict =
        result.value().check({"scripts/kill-decoy.sh", (temp.path() / "data/input.log").string()});

    REQUIRE(verdict.decision == agenticpp::Decision::Allow);
    REQUIRE(verdict.rule_id == "kill-decoy");
}
