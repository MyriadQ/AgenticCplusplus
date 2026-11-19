#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "agenticpp/memory_journal.hpp"
#include "agenticpp/memory_store.hpp"
#include "agenticpp/template_parser.hpp"

namespace {

namespace fs = std::filesystem;

class TempDir {
public:
    explicit TempDir(std::string name) {
        path_ = fs::temp_directory_path() / ("agenticpp_memory_" + std::move(name));
        std::error_code error;
        fs::remove_all(path_, error);
        fs::create_directories(path_, error);
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

agenticpp::MemoryStoreConfig config_for(const TempDir& temp) {
    agenticpp::MemoryStoreConfig config;
    config.snapshot_path = temp.path() / "memory.bin";
    config.journal_path = temp.path() / "memory.journal";
    config.persistent = true;
    config.write_through = true;
    config.compact_after_events = 0;
    return config;
}

agenticpp::FixRecord record_for(std::string purpose, std::string program) {
    agenticpp::FixRecord record;
    record.argv = {std::move(program)};
    record.purpose = std::move(purpose);
    record.successes = 1;
    record.last_seen = std::chrono::system_clock::now();
    return record;
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)), {});
}

}  // namespace

TEST_CASE("MemoryJournal: upsert and outcome survive reload") {
    TempDir temp("journal_reload");
    agenticpp::MemoryJournal journal(temp.path() / "memory.journal");

    const auto first = std::chrono::system_clock::now();
    const auto second = first + std::chrono::seconds{1};
    REQUIRE(journal.append_upsert(42, "bind: <IP>:<NUM>", {"scripts/kill-decoy.sh"},
                                  "free the port", 1, 0, first)
                .has_value());
    REQUIRE(journal.append_outcome(42, 2, 1, second).has_value());

    const auto events = journal.load();
    REQUIRE(events.has_value());
    REQUIRE(events->size() == 2);

    REQUIRE((*events)[0].kind == agenticpp::MemoryJournalEvent::Kind::Upsert);
    REQUIRE((*events)[0].template_id == 42);
    REQUIRE((*events)[0].template_text == "bind: <IP>:<NUM>");
    REQUIRE((*events)[0].argv.size() == 1);
    REQUIRE((*events)[0].argv.front() == "scripts/kill-decoy.sh");
    REQUIRE((*events)[0].purpose == "free the port");

    REQUIRE((*events)[1].kind == agenticpp::MemoryJournalEvent::Kind::Outcome);
    REQUIRE((*events)[1].successes == 2);
    REQUIRE((*events)[1].failures == 1);
}

TEST_CASE("MemoryJournal: truncated trailing line is ignored") {
    TempDir temp("journal_truncated");
    const fs::path path = temp.path() / "memory.journal";
    agenticpp::MemoryJournal journal(path);
    REQUIRE(journal.append_upsert(7, "parse config: <STR>", {"scripts/fix-config.sh"},
                                  "repair config", 1, 0,
                                  std::chrono::system_clock::now())
                .has_value());

    {
        std::ofstream output(path, std::ios::app | std::ios::binary);
        output << R"({"op":"upsert","template_id":8)";
    }

    const auto events = journal.load();
    REQUIRE(events.has_value());
    REQUIRE(events->size() == 1);
    REQUIRE((*events)[0].template_id == 7);
}

TEST_CASE("MemoryJournal: unknown version is rejected") {
    TempDir temp("journal_version");
    const fs::path path = temp.path() / "memory.journal";
    {
        std::ofstream output(path, std::ios::binary);
        output << R"({"version":99})" << '\n';
    }

    agenticpp::MemoryJournal journal(path);
    const auto events = journal.load();
    REQUIRE_FALSE(events.has_value());
    REQUIRE(events.error().message.find("version") != std::string::npos);
}

TEST_CASE("MemoryStore: upsert survives reopen through the journal") {
    TempDir temp("store_reopen");
    const std::string text = "bind: address already in use port=<NUM>";

    {
        agenticpp::MemoryStore store{agenticpp::Vectorizer{}, config_for(temp)};
        REQUIRE(store.upsert(agenticpp::fnv1a64(text), text,
                             record_for("free port 18080", "scripts/kill-decoy.sh"))
                    .has_value());
    }

    agenticpp::MemoryStore reopened{agenticpp::Vectorizer{}, config_for(temp)};
    REQUIRE(reopened.load().has_value());
    REQUIRE(reopened.size() == 1);

    const auto hits = reopened.search(text);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits.front().template_id == agenticpp::fnv1a64(text));
    REQUIRE(hits.front().score == Catch::Approx(1.0f).margin(1e-5f));
    REQUIRE(hits.front().record->argv.front() == "scripts/kill-decoy.sh");
    REQUIRE(hits.front().record->purpose == "free port 18080");
}

TEST_CASE("MemoryStore: outcome counters survive reopen") {
    TempDir temp("store_outcome");
    const std::string text = "bind: address already in use";
    const std::uint64_t id = agenticpp::fnv1a64(text);

    {
        agenticpp::MemoryStore store{agenticpp::Vectorizer{}, config_for(temp)};
        REQUIRE(store.upsert(id, text, record_for("known", "scripts/kill-decoy.sh")).has_value());
        REQUIRE(store.record_outcome(id, true).has_value());
        REQUIRE(store.record_outcome(id, true).has_value());
        REQUIRE(store.record_outcome(id, false).has_value());
    }

    agenticpp::MemoryStore reopened{agenticpp::Vectorizer{}, config_for(temp)};
    REQUIRE(reopened.load().has_value());
    const auto hits = reopened.search(text);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits.front().record->successes == 3);
    REQUIRE(hits.front().record->failures == 1);
}

TEST_CASE("MemoryStore: duplicate upsert stays one record after reload") {
    TempDir temp("store_duplicate");
    const std::string text = "bind: address already in use";
    const std::uint64_t id = agenticpp::fnv1a64(text);

    {
        agenticpp::MemoryStore store{agenticpp::Vectorizer{}, config_for(temp)};
        REQUIRE(store.upsert(id, text, record_for("first", "scripts/restart-victim.sh"))
                    .has_value());
        REQUIRE(store.upsert(id, text, record_for("second", "scripts/kill-decoy.sh"))
                    .has_value());
    }

    agenticpp::MemoryStore reopened{agenticpp::Vectorizer{}, config_for(temp)};
    REQUIRE(reopened.load().has_value());
    REQUIRE(reopened.size() == 1);
    const auto hits = reopened.search(text);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits.front().record->purpose == "second");
}

TEST_CASE("MemoryStore: compact writes a snapshot and clears the journal") {
    TempDir temp("store_compact");
    const std::string text = "bind: address already in use";
    const std::uint64_t id = agenticpp::fnv1a64(text);

    {
        agenticpp::MemoryStore store{agenticpp::Vectorizer{}, config_for(temp)};
        REQUIRE(store.upsert(id, text, record_for("free port", "scripts/kill-decoy.sh"))
                    .has_value());
        REQUIRE(store.compact().has_value());
    }

    REQUIRE(fs::exists(temp.path() / "memory.bin"));
    REQUIRE(read_file(temp.path() / "memory.journal").find("upsert") == std::string::npos);

    agenticpp::MemoryStore reopened{agenticpp::Vectorizer{}, config_for(temp)};
    REQUIRE(reopened.load().has_value());
    REQUIRE(reopened.size() == 1);
    const auto hits = reopened.search(text);
    REQUIRE(hits.size() == 1);
    REQUIRE(hits.front().record->purpose == "free port");
}

TEST_CASE("MemoryStore: snapshot reload works without a journal") {
    TempDir temp("store_snapshot_only");
    const std::string text = "parse config: unexpected token";
    const std::uint64_t id = agenticpp::fnv1a64(text);

    {
        agenticpp::MemoryStore store{agenticpp::Vectorizer{}, config_for(temp)};
        REQUIRE(store.upsert(id, text, record_for("repair config", "scripts/fix-config.sh"))
                    .has_value());
        REQUIRE(store.compact().has_value());
    }
    REQUIRE(fs::remove(temp.path() / "memory.journal"));

    agenticpp::MemoryStore reopened{agenticpp::Vectorizer{}, config_for(temp)};
    REQUIRE(reopened.load().has_value());
    REQUIRE(reopened.size() == 1);
    REQUIRE(reopened.search(text).size() == 1);
}

TEST_CASE("MemoryStore: corrupt snapshot leaves existing memory unchanged") {
    TempDir temp("store_corrupt_snapshot");
    const std::string text = "bind: address already in use";
    const std::uint64_t id = agenticpp::fnv1a64(text);

    {
        std::ofstream output(temp.path() / "memory.bin", std::ios::binary);
        output << "not a snapshot";
    }

    agenticpp::MemoryStore store{agenticpp::Vectorizer{}, config_for(temp)};
    REQUIRE(store.upsert(id, text, record_for("still here", "scripts/kill-decoy.sh"))
                .has_value());
    REQUIRE(store.size() == 1);

    const auto loaded = store.load();
    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(store.size() == 1);
    REQUIRE(store.search(text).front().record->purpose == "still here");
}

TEST_CASE("MemoryStore: auto-compacts when the journal threshold is reached") {
    TempDir temp("store_auto_compact");
    agenticpp::MemoryStoreConfig config = config_for(temp);
    config.compact_after_events = 2;

    {
        agenticpp::MemoryStore store{agenticpp::Vectorizer{}, config};
        REQUIRE(store.upsert(1, "bind: address already in use",
                             record_for("first", "scripts/kill-decoy.sh"))
                    .has_value());
        REQUIRE(store.upsert(2, "parse config: unexpected token",
                             record_for("second", "scripts/fix-config.sh"))
                    .has_value());
    }

    REQUIRE(fs::exists(temp.path() / "memory.bin"));
    REQUIRE(read_file(temp.path() / "memory.journal").find("upsert") == std::string::npos);

    agenticpp::MemoryStore reopened{agenticpp::Vectorizer{}, config};
    REQUIRE(reopened.load().has_value());
    REQUIRE(reopened.size() == 2);
}

TEST_CASE("MemoryStore: read-only store loads but does not append") {
    TempDir temp("store_read_only");
    const std::string text = "bind: address already in use";
    const std::uint64_t id = agenticpp::fnv1a64(text);

    {
        agenticpp::MemoryStore store{agenticpp::Vectorizer{}, config_for(temp)};
        REQUIRE(store.upsert(id, text, record_for("stored", "scripts/kill-decoy.sh"))
                    .has_value());
        REQUIRE(store.compact().has_value());
    }

    const std::string before = read_file(temp.path() / "memory.journal");
    agenticpp::MemoryStoreConfig read_only = config_for(temp);
    read_only.write_through = false;
    agenticpp::MemoryStore store{agenticpp::Vectorizer{}, read_only};
    REQUIRE(store.load().has_value());
    REQUIRE(store.upsert(agenticpp::fnv1a64("new text"), "new text",
                         record_for("not persisted", "scripts/restart-victim.sh"))
                .has_value());
    REQUIRE(store.size() == 2);
    REQUIRE(read_file(temp.path() / "memory.journal") == before);
}
