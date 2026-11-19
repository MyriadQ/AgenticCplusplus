#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "agenticpp/skills.hpp"
#include "agenticpp/template_parser.hpp"

namespace {

namespace fs = std::filesystem;

class TempDir {
public:
    explicit TempDir(std::string name) {
        path_ = fs::temp_directory_path() / ("agenticpp_skills_" + std::move(name));
        std::error_code error;
        fs::remove_all(path_, error);
        fs::create_directories(path_ / "run", error);
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

int listening_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(fd, 1) == 0);
    socklen_t length = sizeof(address);
    REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    return static_cast<int>(ntohs(address.sin_port));
}

bool has_evidence(const agenticpp::SkillResult& result, std::string_view source,
                  std::string_view fragment = {}) {
    for (const agenticpp::Evidence& evidence : result.evidence) {
        if (evidence.source == source &&
            (fragment.empty() || evidence.detail.find(fragment) != std::string::npos)) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("Skills: port conflict identifies a live decoy") {
    TempDir temp("port");
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    REQUIRE(::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(fd, 1) == 0);
    socklen_t length = sizeof(address);
    REQUIRE(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    const int port = static_cast<int>(ntohs(address.sin_port));

    std::ofstream(temp.path() / "run" / "decoy.pid") << ::getpid() << '\n';

    const std::string line =
        "error service=victim bind: address already in use port=" + std::to_string(port);
    const agenticpp::ParsedLine parsed = agenticpp::parse_template(line);
    agenticpp::SkillConfig config;
    config.project_root = temp.path();
    config.run_dir = temp.path() / "run";
    config.default_port = port;
    agenticpp::SkillRegistry registry(config);

    const auto result = registry.probe(agenticpp::SkillContext{line, parsed});
    REQUIRE(result.conclusive);
    REQUIRE(result.cause == "port_conflict");
    REQUIRE(result.suggested_argv.size() == 1);
    REQUIRE(result.suggested_argv.front() == "scripts/kill-decoy.sh");
    REQUIRE(has_evidence(result, "port_conflict", "LISTEN"));
    REQUIRE(has_evidence(result, "port_conflict", "decoy pid"));

    ::close(fd);
}

TEST_CASE("Skills: invalid config is diagnosed from a captured path") {
    TempDir temp("config");
    const fs::path config_path = temp.path() / "bad.json";
    std::ofstream(config_path) << R"({"broken": )";

    const std::string line =
        "parse config: unexpected token in " + config_path.string();
    const agenticpp::ParsedLine parsed = agenticpp::parse_template(line);
    agenticpp::SkillConfig config;
    config.project_root = temp.path();
    config.run_dir = temp.path() / "run";
    config.default_port = 1;
    agenticpp::SkillRegistry registry(config);

    const auto result = registry.probe(agenticpp::SkillContext{line, parsed});
    REQUIRE(result.conclusive);
    REQUIRE(result.cause == "config_invalid");
    REQUIRE(result.suggested_argv.front() == "scripts/fix-config.sh");
    REQUIRE(has_evidence(result, "config_invalid", "failed JSON parse"));
}

TEST_CASE("Skills: missing service state suggests a restart") {
    TempDir temp("service");
    const std::string line = "error service=victim connection refused";
    const agenticpp::ParsedLine parsed = agenticpp::parse_template(line);
    agenticpp::SkillConfig config;
    config.project_root = temp.path();
    config.run_dir = temp.path() / "run";
    config.default_port = 1;
    agenticpp::SkillRegistry registry(config);

    const auto result = registry.probe(agenticpp::SkillContext{line, parsed});
    REQUIRE(result.conclusive);
    REQUIRE(result.cause == "service_down");
    REQUIRE(result.suggested_argv.front() == "scripts/restart-victim.sh");
}

TEST_CASE("Skills: recent occurrences counts matching templates") {
    TempDir temp("recent");
    const std::string line = "error service=victim unknown failure";
    const agenticpp::ParsedLine parsed = agenticpp::parse_template(line);
    const fs::path log = temp.path() / "victim.log";
    std::ofstream(log) << line << '\n' << line << '\n';
    std::ofstream(temp.path() / "run" / "victim.pid") << ::getpid() << '\n';

    agenticpp::SkillConfig config;
    config.project_root = temp.path();
    config.log_path = log;
    config.run_dir = temp.path() / "run";
    config.default_port = 1;
    agenticpp::SkillRegistry registry(config);

    const auto result = registry.probe(agenticpp::SkillContext{line, parsed});
    REQUIRE(has_evidence(result, "recent_occurrences", "2 time(s)"));
}
