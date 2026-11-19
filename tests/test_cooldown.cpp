#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>

#include "agenticpp/cooldown.hpp"

namespace {

agenticpp::AgentConfig config() {
    agenticpp::AgentConfig value;
    value.dedup_window = std::chrono::seconds{30};
    value.cooldown_initial = std::chrono::seconds{10};
    value.cooldown_max = std::chrono::seconds{40};
    return value;
}

agenticpp::Timestamp at(int seconds) {
    return agenticpp::Timestamp{std::chrono::seconds{seconds}};
}

}  // namespace

TEST_CASE("CooldownTable: claims one active incident per template") {
    agenticpp::CooldownTable table(config());

    const auto first = table.try_begin(42, at(0));
    REQUIRE(first.allow);

    const auto second = table.try_begin(42, at(0));
    REQUIRE_FALSE(second.allow);
    REQUIRE(second.reason.find("active") != std::string::npos);

    const auto other = table.try_begin(43, at(0));
    REQUIRE(other.allow);
}

TEST_CASE("CooldownTable: failures back off exponentially") {
    agenticpp::CooldownTable table(config());

    REQUIRE(table.try_begin(7, at(0)).allow);
    table.finish_failure(7, at(0));

    REQUIRE_FALSE(table.try_begin(7, at(1)).allow);
    REQUIRE(table.try_begin(7, at(11)).allow);
    table.finish_failure(7, at(11));

    REQUIRE_FALSE(table.try_begin(7, at(12)).allow);
    REQUIRE(table.try_begin(7, at(32)).allow);
    table.finish_failure(7, at(32));

    REQUIRE_FALSE(table.try_begin(7, at(33)).allow);
    REQUIRE(table.try_begin(7, at(73)).allow);
}

TEST_CASE("CooldownTable: success uses the dedup window") {
    agenticpp::CooldownTable table(config());

    REQUIRE(table.try_begin(9, at(0)).allow);
    table.finish_success(9, at(0));

    REQUIRE_FALSE(table.try_begin(9, at(1)).allow);
    const auto later = table.try_begin(9, at(31));
    REQUIRE(later.allow);
    REQUIRE(table.size() == 1);
}
