#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "agenticpp/result.hpp"
#include "agenticpp/types.hpp"

namespace agenticpp {

// One durable event in the append-only known-bug journal.
// Outcome events carry absolute counters so replaying an old journal over a
// newer snapshot is idempotent.
struct MemoryJournalEvent {
    enum class Kind {
        Upsert,
        Outcome,
    };

    Kind kind = Kind::Upsert;
    std::uint64_t template_id = 0;
    std::string template_text;
    std::vector<std::string> argv;
    std::string purpose;
    std::uint32_t successes = 0;
    std::uint32_t failures = 0;
    Timestamp last_seen{};
};

// Append-only JSONL journal. The first line is {"version":1}. Every append is
// followed by fsync so a completed repair survives a crash.
class MemoryJournal {
public:
    MemoryJournal() = default;
    explicit MemoryJournal(std::filesystem::path path);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    Result<void> append_upsert(std::uint64_t template_id, std::string_view template_text,
                               const std::vector<std::string>& argv, std::string_view purpose,
                               std::uint32_t successes, std::uint32_t failures,
                               Timestamp last_seen);
    Result<void> append_outcome(std::uint64_t template_id, std::uint32_t successes,
                                std::uint32_t failures, Timestamp last_seen);
    Result<std::vector<MemoryJournalEvent>> load() const;
    Result<void> clear();

private:
    Result<void> append_line(std::string line);

    std::filesystem::path path_;
};

}  // namespace agenticpp
