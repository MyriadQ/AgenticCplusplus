#include "agenticpp/memory_store.hpp"

#include <utility>

#include "agenticpp/template_parser.hpp"

namespace agenticpp {

void MemoryStore::upsert(std::uint64_t id, std::string_view text, FixRecord record) {
    record.template_text = std::string(text);
    records_.insert_or_assign(id, std::move(record));
}

void MemoryStore::record_outcome(std::uint64_t id, bool success) {
    const auto it = records_.find(id);
    if (it == records_.end()) {
        return;
    }

    if (success) {
        ++it->second.successes;
    } else {
        ++it->second.failures;
    }
}

std::vector<Hit> MemoryStore::search(std::string_view query, std::size_t k) const {
    std::vector<Hit> hits;
    if (k == 0) {
        return hits;
    }

    const std::uint64_t id = fnv1a64(query);
    const auto it = records_.find(id);
    if (it == records_.end() || it->second.template_text != query) {
        return hits;
    }

    hits.push_back(Hit{id, 1.0f, &it->second});
    return hits;
}

std::size_t MemoryStore::size() const noexcept {
    return records_.size();
}

}  // namespace agenticpp
