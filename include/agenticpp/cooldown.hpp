#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

#include "agenticpp/types.hpp"

namespace agenticpp {

struct CooldownDecision {
    bool allow = false;
    std::string reason;
};

// Shared across Agent workers. It prevents the same template from being
// repaired concurrently, suppresses duplicates inside the dedup window, and
// backs off exponentially after failed attempts.
class CooldownTable {
public:
    explicit CooldownTable(AgentConfig config);

    CooldownDecision try_begin(std::uint64_t template_id, Timestamp now);
    void finish_success(std::uint64_t template_id, Timestamp now);
    void finish_failure(std::uint64_t template_id, Timestamp now);

    [[nodiscard]] std::size_t size() const;

private:
    struct Entry {
        Timestamp cooldown_until{};
        Timestamp dedup_until{};
        std::uint32_t failures = 0;
        bool active = false;
    };

    AgentConfig config_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, Entry> entries_;
};

}  // namespace agenticpp
