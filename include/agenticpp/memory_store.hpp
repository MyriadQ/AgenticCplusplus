#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "agenticpp/types.hpp"

namespace agenticpp {

struct FixRecord {
    std::string template_text;
    std::vector<std::string> argv;
    std::string purpose;
    std::uint32_t successes = 0;
    std::uint32_t failures = 0;
    Timestamp last_seen{};
};

// `record` is borrowed: valid while the store is alive and the record is not erased.
// upsert() updates in place and does not erase, so a Hit may outlive the search call.
struct Hit {
    std::uint64_t template_id = 0;
    float score = 0.0f;
    const FixRecord* record = nullptr;
};

class MemoryStore {
public:
    static constexpr std::size_t kDefaultTopK = 3;

    // The caller supplies the template id (normally ParsedLine::template_id).
    // Stage 1 is exact-match only; Stage 2 replaces the internals with vector search.
    void upsert(std::uint64_t id, std::string_view text, FixRecord record);
    void record_outcome(std::uint64_t id, bool success);

    [[nodiscard]] std::vector<Hit> search(std::string_view query,
                                          std::size_t k = kDefaultTopK) const;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::unordered_map<std::uint64_t, FixRecord> records_;
};

}  // namespace agenticpp
