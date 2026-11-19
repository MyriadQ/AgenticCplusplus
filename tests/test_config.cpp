#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>

#include "agenticpp/config.hpp"

namespace {

std::filesystem::path source_root() {
    return std::filesystem::path{AGENTICPP_SOURCE_DIR};
}

}  // namespace

TEST_CASE("Config: loads the agent configuration") {
    const auto config = agenticpp::load_config(source_root() / "config/agent.json");

    REQUIRE(config.has_value());
    REQUIRE(config.value().mode == "dry_run");
    REQUIRE(config.value().require_confirmation);
    REQUIRE(config.value().executor.timeout == std::chrono::milliseconds{30000});
    REQUIRE(config.value().executor.grace == std::chrono::milliseconds{200});
    REQUIRE(config.value().executor.output_cap == 64 * 1024);
    REQUIRE(std::find(config.value().executor.env_allowlist.begin(),
                      config.value().executor.env_allowlist.end(),
                      "PATH") != config.value().executor.env_allowlist.end());
    REQUIRE(config.value().executor.limits.nproc == 4096);
    REQUIRE(config.value().verifier.health_url.find("18080") != std::string::npos);
    REQUIRE(config.value().verifier.health_timeout == std::chrono::milliseconds{5000});
    REQUIRE(config.value().llm.model == "deepseek-flash");
    REQUIRE(config.value().llm.timeout_ms == 15000);
    REQUIRE(config.value().agent.max_tool_calls == 4);
    REQUIRE(config.value().agent.max_llm_round_trips == 3);
    REQUIRE(config.value().agent.max_concurrent_incidents == 2);
    REQUIRE(config.value().agent.dedup_window == std::chrono::seconds{60});
    REQUIRE(config.value().agent.recall_threshold == Catch::Approx(0.86f));
    REQUIRE(config.value().memory.dim == 512);
    REQUIRE(config.value().memory.top_k == 3);
    REQUIRE(config.value().memory.journal_path == "data/memory.journal");
    REQUIRE(config.value().memory.compact_after_events == 1024);
    REQUIRE(config.value().executor.sandbox_tier == 0);
    REQUIRE_FALSE(config.value().executor.sandbox_network);
    REQUIRE(config.value().executor.sandbox_writable_paths.size() == 2);
}

TEST_CASE("Config: missing file is an error") {
    const auto config = agenticpp::load_config(source_root() / "config/no-such-file.json");

    REQUIRE_FALSE(config.has_value());
    REQUIRE(config.error().code != 0);
}
