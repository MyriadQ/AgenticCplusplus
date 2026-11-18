#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <vector>

#include "agenticpp/types.hpp"

namespace {

class RecordingSink final : public agenticpp::EventSink {
public:
    void emit(const agenticpp::Event& event) override { events.push_back(event); }

    std::vector<agenticpp::Event> events;
};

}  // namespace

TEST_CASE("Phase: transition records from/to/reason") {
    const agenticpp::Transition transition{
        agenticpp::Phase::Detect,
        agenticpp::Phase::Recall,
        "incident detected",
        agenticpp::Timestamp{},
    };

    REQUIRE(transition.from == agenticpp::Phase::Detect);
    REQUIRE(transition.to == agenticpp::Phase::Recall);
    REQUIRE(transition.reason == "incident detected");
}

TEST_CASE("Phase: names are stable") {
    REQUIRE(agenticpp::phase_name(agenticpp::Phase::Idle) == "Idle");
    REQUIRE(agenticpp::phase_name(agenticpp::Phase::Detect) == "Detect");
    REQUIRE(agenticpp::phase_name(agenticpp::Phase::Recall) == "Recall");
    REQUIRE(agenticpp::phase_name(agenticpp::Phase::Diagnose) == "Diagnose");
    REQUIRE(agenticpp::phase_name(agenticpp::Phase::Propose) == "Propose");
    REQUIRE(agenticpp::phase_name(agenticpp::Phase::Authorize) == "Authorize");
    REQUIRE(agenticpp::phase_name(agenticpp::Phase::Act) == "Act");
    REQUIRE(agenticpp::phase_name(agenticpp::Phase::Verify) == "Verify");
    REQUIRE(agenticpp::phase_name(agenticpp::Phase::Learn) == "Learn");
    REQUIRE(agenticpp::phase_name(agenticpp::Phase::Cooldown) == "Cooldown");
    REQUIRE(agenticpp::phase_name(agenticpp::Phase::Done) == "Done");
}

TEST_CASE("EventSink: receives emitted events") {
    RecordingSink sink;
    sink.emit(agenticpp::Event{agenticpp::Timestamp{}, "Detect", "bind: address already in use"});

    REQUIRE(sink.events.size() == 1);
    REQUIRE(sink.events.front().phase == "Detect");
    REQUIRE(sink.events.front().detail == "bind: address already in use");
}

TEST_CASE("AgentConfig: budgets have safe defaults") {
    const agenticpp::AgentConfig config;

    REQUIRE(config.max_tool_calls == 4);
    REQUIRE(config.max_llm_round_trips == 3);
    REQUIRE(config.max_concurrent_incidents == 2);
    REQUIRE(config.recall_threshold == 0.86f);
    REQUIRE(config.dedup_window == std::chrono::seconds{60});
    REQUIRE(config.cooldown_max == std::chrono::seconds{1800});
}
