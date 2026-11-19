#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "agenticpp/memory_journal.hpp"
#include "agenticpp/result.hpp"
#include "agenticpp/types.hpp"
#include "agenticpp/vectorizer.hpp"

namespace agenticpp {

struct FixRecord {
    std::string template_text;
    std::vector<std::string> argv;
    std::string purpose;
    std::uint32_t successes = 0;
    std::uint32_t failures = 0;
    Timestamp last_seen{};
};

// `owner` keeps the snapshot alive even if the store is mutated concurrently.
// `record` is the same object as `owner.get()`, retained for the original
// borrowed-pointer API.
struct Hit {
    std::uint64_t template_id = 0;
    float score = 0.0f;
    const FixRecord* record = nullptr;
    std::shared_ptr<const FixRecord> owner;
};

struct MemoryStoreConfig {
    std::filesystem::path snapshot_path;
    std::filesystem::path journal_path;
    bool persistent = false;
    bool write_through = true;
    std::size_t compact_after_events = 1024;
};

class MemoryStore {
public:
    static constexpr std::size_t kDefaultTopK = 3;

    MemoryStore();
    explicit MemoryStore(std::size_t reserve);
    explicit MemoryStore(const Vectorizer& vectorizer, std::size_t reserve = 1024);
    MemoryStore(const Vectorizer& vectorizer, MemoryStoreConfig config,
                std::size_t reserve = 1024);

    // Loads snapshot then journal into temporary state and swaps only on
    // success. A corrupt file leaves the current store unchanged.
    Result<void> load();

    // The caller supplies the template id (normally ParsedLine::template_id).
    // The vector is recomputed from text.
    Result<void> upsert(std::uint64_t id, std::string_view text, FixRecord record);
    Result<void> record_outcome(std::uint64_t id, bool success);

    [[nodiscard]] std::vector<Hit> search(std::string_view query,
                                          std::size_t k = kDefaultTopK) const;
    [[nodiscard]] std::size_t size() const noexcept;

    // Writes an atomic snapshot and truncates the journal. A no-op for a
    // non-persistent or read-only store.
    Result<void> compact();
    void set_write_through(bool enabled);

    [[nodiscard]] const MemoryStoreConfig& config() const noexcept { return config_; }

private:
    Result<void> maybe_compact_locked();
    Result<void> compact_locked();

    Vectorizer vectorizer_;
    MemoryStoreConfig config_;
    MemoryJournal journal_;

    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<const FixRecord>> records_;
    std::vector<std::uint64_t> ids_;
    std::vector<float> vectors_;
    std::unordered_map<std::uint64_t, std::size_t> row_by_id_;
    std::size_t journal_events_ = 0;
};

}  // namespace agenticpp
