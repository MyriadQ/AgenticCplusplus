#include "agenticpp/cooldown.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

namespace agenticpp {
namespace {

std::string template_name(std::uint64_t template_id) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llx",
                  static_cast<unsigned long long>(template_id));
    return buffer;
}

std::int64_t remaining_seconds(Timestamp until, Timestamp now) {
    if (until <= now) {
        return 0;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::seconds>(until - now);
    return remaining.count() + 1;
}

std::chrono::seconds failure_delay(const AgentConfig& config, std::uint32_t failures) {
    std::int64_t delay = config.cooldown_initial.count();
    if (delay <= 0) {
        return std::chrono::seconds{0};
    }
    const std::int64_t maximum = std::max<std::int64_t>(delay, config.cooldown_max.count());
    for (std::uint32_t i = 1; i < failures && delay < maximum; ++i) {
        delay = std::min<std::int64_t>(maximum, delay * 2);
    }
    return std::chrono::seconds{delay};
}

}  // namespace

CooldownTable::CooldownTable(AgentConfig config) : config_(std::move(config)) {}

CooldownDecision CooldownTable::try_begin(std::uint64_t template_id, Timestamp now) {
    std::lock_guard<std::mutex> lock(mutex_);
    constexpr std::size_t kPruneThreshold = 4096;
    if (entries_.size() > kPruneThreshold) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (!it->second.active && it->second.cooldown_until <= now &&
                it->second.dedup_until <= now) {
                it = entries_.erase(it);
            } else {
                ++it;
            }
        }
    }
    Entry& entry = entries_[template_id];
    if (entry.active) {
        return CooldownDecision{false, "template " + template_name(template_id) +
                                           " already has an active repair"};
    }
    if (entry.cooldown_until > now) {
        return CooldownDecision{
            false, "template " + template_name(template_id) + " in cooldown for " +
                       std::to_string(remaining_seconds(entry.cooldown_until, now)) + "s"};
    }
    if (entry.dedup_until > now) {
        return CooldownDecision{
            false, "template " + template_name(template_id) + " deduplicated for " +
                       std::to_string(remaining_seconds(entry.dedup_until, now)) + "s"};
    }
    entry.active = true;
    return CooldownDecision{true, "claimed template " + template_name(template_id)};
}

void CooldownTable::finish_success(std::uint64_t template_id, Timestamp now) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& entry = entries_[template_id];
    entry.active = false;
    entry.failures = 0;
    entry.cooldown_until = Timestamp{};
    entry.dedup_until = config_.dedup_window.count() > 0 ? now + config_.dedup_window : now;
}

void CooldownTable::finish_failure(std::uint64_t template_id, Timestamp now) {
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& entry = entries_[template_id];
    entry.active = false;
    ++entry.failures;
    entry.cooldown_until = now + failure_delay(config_, entry.failures);
    entry.dedup_until = Timestamp{};
}

std::size_t CooldownTable::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

}  // namespace agenticpp
