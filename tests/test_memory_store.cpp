#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

TEST_CASE("MemoryStore: exact query ranks the fix first") {
    const std::string text = "bind: address already in use";
    const std::uint64_t id = agenticpp::fnv1a64(text);

    agenticpp::MemoryStore store;
    store.upsert(id, text, make_record("free port 8080", "scripts/kill-decoy.sh"));

    const auto hits = store.search(text);

    REQUIRE(hits.size() == 1);
    REQUIRE(hits.front().template_id == id);
    REQUIRE(hits.front().score == Catch::Approx(1.0f).margin(1e-6f));
    REQUIRE(hits.front().record != nullptr);
    REQUIRE(hits.front().record->argv.size() == 1);
    REQUIRE(hits.front().record->argv.front() == "scripts/kill-decoy.sh");
    REQUIRE(hits.front().record->purpose == "free port 8080");
}

TEST_CASE("MemoryStore: unrelated query scores below recall threshold") {
    const std::string text = "bind: address already in use";
    const std::uint64_t id = agenticpp::fnv1a64(text);

    agenticpp::MemoryStore store;
    store.upsert(id, text, make_record("free port 8080", "scripts/kill-decoy.sh"));

    const auto hits = store.search("parse config: unexpected token");

    REQUIRE(hits.size() == 1);
    REQUIRE(hits.front().score < 0.86f);
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

TEST_CASE("MemoryStore: id is retained in hits") {
    const std::string text = "bind: address already in use";

    agenticpp::MemoryStore store;
    store.upsert(12345, text, make_record("free port 8080", "scripts/kill-decoy.sh"));

    const auto hits = store.search(text);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits.front().template_id == 12345);
    REQUIRE(hits.front().score > 0.99f);
}

TEST_CASE("MemoryStore: related template ranks above unrelated") {
    const std::string related = "bind: address already in use";
    const std::string unrelated = "parse config: unexpected token";

    agenticpp::MemoryStore store;
    store.upsert(agenticpp::fnv1a64(related), related,
                 make_record("free port 8080", "scripts/kill-decoy.sh"));
    store.upsert(agenticpp::fnv1a64(unrelated), unrelated,
                 make_record("repair config", "scripts/fix-config.sh"));

    const auto hits = store.search(related, 2);
    REQUIRE(hits.size() == 2);
    REQUIRE(hits.front().template_id == agenticpp::fnv1a64(related));
    REQUIRE(hits.front().score > hits.back().score);
}

TEST_CASE("MemoryStore: top-k scores are descending") {
    const std::string first = "bind: address already in use";
    const std::string second = "bind: permission denied";
    const std::string third = "parse config: unexpected token";

    agenticpp::MemoryStore store;
    store.upsert(agenticpp::fnv1a64(first), first, make_record("a", "scripts/a.sh"));
    store.upsert(agenticpp::fnv1a64(second), second, make_record("b", "scripts/b.sh"));
    store.upsert(agenticpp::fnv1a64(third), third, make_record("c", "scripts/c.sh"));

    const auto hits = store.search("bind: address already in use", 2);
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0].score >= hits[1].score);
}

TEST_CASE("MemoryStore: empty query returns no hits") {
    const std::string text = "bind: address already in use";

    agenticpp::MemoryStore store;
    store.upsert(agenticpp::fnv1a64(text), text,
                 make_record("free port 8080", "scripts/kill-decoy.sh"));

    REQUIRE(store.search("").empty());
}

TEST_CASE("MemoryStore: concurrent search and mutation stay consistent") {
    agenticpp::MemoryStore store;
    const std::string seed = "bind: address already in use";
    const std::uint64_t seed_id = agenticpp::fnv1a64(seed);
    REQUIRE(store.upsert(seed_id, seed,
                         make_record("free port 8080", "scripts/kill-decoy.sh"))
                .has_value());

    std::vector<std::thread> threads;
    for (int thread = 0; thread < 4; ++thread) {
        threads.emplace_back([&store, thread, &seed] {
            for (int i = 0; i < 50; ++i) {
                const std::string text =
                    "bind: address already in use worker=" + std::to_string(thread) +
                    " iteration=" + std::to_string(i);
                const std::uint64_t id = agenticpp::fnv1a64(text);
                (void)store.upsert(id, text,
                                   make_record("concurrent", "scripts/kill-decoy.sh"));
                (void)store.record_outcome(id, true);
                (void)store.search(seed, 3);
                (void)store.size();
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    REQUIRE(store.size() == 201);
    REQUIRE(store.search(seed).front().template_id == seed_id);
}

TEST_CASE("MemoryStore: outcome counters break equal-score ties") {
    const std::string text = "bind: address already in use";

    agenticpp::MemoryStore store;
    store.upsert(1, text, make_record("unreliable", "scripts/restart-victim.sh"));
    store.upsert(2, text, make_record("reliable", "scripts/kill-decoy.sh"));
    store.record_outcome(1, false);
    store.record_outcome(2, true);

    const auto hits = store.search(text, 2);
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0].template_id == 2);
    REQUIRE(hits[1].template_id == 1);
}
