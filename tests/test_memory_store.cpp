#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>

#include "agenticpp/memory_store.hpp"
#include "agenticpp/template_parser.hpp"

namespace {

agenticpp::FixRecord make_record(std::string purpose, std::string program) {
    agenticpp::FixRecord record;
    record.argv = {std::move(program)};
    record.purpose = std::move(purpose);
    return record;
}

}  // namespace

TEST_CASE("MemoryStore: exact match returns the fix") {
    const std::string text = "bind: address already in use";
    const std::uint64_t id = agenticpp::fnv1a64(text);

    agenticpp::MemoryStore store;
    store.upsert(id, text, make_record("free port 8080", "scripts/kill-decoy.sh"));

    const auto hits = store.search(text);

    REQUIRE(hits.size() == 1);
    REQUIRE(hits.front().template_id == id);
    REQUIRE(hits.front().score == 1.0f);
    REQUIRE(hits.front().record != nullptr);
    REQUIRE(hits.front().record->argv.size() == 1);
    REQUIRE(hits.front().record->argv.front() == "scripts/kill-decoy.sh");
    REQUIRE(hits.front().record->purpose == "free port 8080");
}

TEST_CASE("MemoryStore: miss returns no hits") {
    const std::string text = "bind: address already in use";
    const std::uint64_t id = agenticpp::fnv1a64(text);

    agenticpp::MemoryStore store;
    store.upsert(id, text, make_record("free port 8080", "scripts/kill-decoy.sh"));

    REQUIRE(store.search("different failure").empty());
}

TEST_CASE("MemoryStore: upsert twice does not duplicate") {
    const std::string text = "bind: address already in use";
    const std::uint64_t id = agenticpp::fnv1a64(text);

    agenticpp::MemoryStore store;
    store.upsert(id, text, make_record("first", "scripts/restart-victim.sh"));
    store.upsert(id, text, make_record("second", "scripts/kill-decoy.sh"));

    REQUIRE(store.size() == 1);

    const auto hits = store.search(text);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits.front().record->purpose == "second");
    REQUIRE(hits.front().record->argv.front() == "scripts/kill-decoy.sh");
}

TEST_CASE("MemoryStore: record outcome updates counters") {
    const std::string text = "bind: address already in use";
    const std::uint64_t id = agenticpp::fnv1a64(text);

    agenticpp::MemoryStore store;
    store.upsert(id, text, make_record("free port 8080", "scripts/kill-decoy.sh"));

    store.record_outcome(id, true);
    store.record_outcome(id, true);
    store.record_outcome(id, false);

    const auto hits = store.search(text);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits.front().record->successes == 2);
    REQUIRE(hits.front().record->failures == 1);
}

TEST_CASE("MemoryStore: search respects k") {
    const std::string text = "bind: address already in use";
    const std::uint64_t id = agenticpp::fnv1a64(text);

    agenticpp::MemoryStore store;
    store.upsert(id, text, make_record("free port 8080", "scripts/kill-decoy.sh"));

    REQUIRE(store.search(text, 0).empty());
    REQUIRE(store.search(text, 1).size() == 1);
}

TEST_CASE("MemoryStore: hit record stays valid after another upsert") {
    const std::string first_text = "bind: address already in use";
    const std::string second_text = "parse config: unexpected token";

    agenticpp::MemoryStore store;
    store.upsert(agenticpp::fnv1a64(first_text), first_text,
                 make_record("free port 8080", "scripts/kill-decoy.sh"));

    const auto first_hits = store.search(first_text);
    const agenticpp::FixRecord* borrowed = first_hits.front().record;
    REQUIRE(borrowed != nullptr);

    store.upsert(agenticpp::fnv1a64(second_text), second_text,
                 make_record("repair config", "scripts/fix-config.sh"));

    REQUIRE(store.size() == 2);
    REQUIRE(borrowed->argv.front() == "scripts/kill-decoy.sh");
}

TEST_CASE("MemoryStore: id must match the template text") {
    const std::string text = "bind: address already in use";

    agenticpp::MemoryStore store;
    store.upsert(12345, text, make_record("free port 8080", "scripts/kill-decoy.sh"));

    REQUIRE(store.search(text).empty());
}
