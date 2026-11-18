#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "agenticpp/event_sink.hpp"
#include "recording_sink.hpp"

TEST_CASE("EventSink: tee forwards to both sinks") {
    agenticpp::RecordingSink first;
    agenticpp::RecordingSink second;
    agenticpp::TeeSink tee(first, second);

    tee.emit(agenticpp::Event{agenticpp::Timestamp{}, "Detect", "incident"});

    REQUIRE(first.events.size() == 1);
    REQUIRE(second.events.size() == 1);
    REQUIRE(first.events.front().phase == "Detect");
    REQUIRE(second.events.front().detail == "incident");
}

TEST_CASE("EventSink: JSONL sink writes one object per event") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / "agenticpp_events_test.jsonl";
    std::filesystem::remove(path);

    {
        agenticpp::JsonlSink sink(path);
        sink.emit(agenticpp::Event{agenticpp::Timestamp{}, "Recall", "miss"});
        sink.emit(agenticpp::Event{agenticpp::Timestamp{}, "Learn", "stored"});
    }

    std::ifstream input(path);
    REQUIRE(input.good());

    std::string line;
    REQUIRE(std::getline(input, line));
    const auto first = nlohmann::json::parse(line);
    REQUIRE(first["phase"] == "Recall");
    REQUIRE(first["detail"] == "miss");

    REQUIRE(std::getline(input, line));
    const auto second = nlohmann::json::parse(line);
    REQUIRE(second["phase"] == "Learn");
    REQUIRE(second["detail"] == "stored");

    std::filesystem::remove(path);
}
