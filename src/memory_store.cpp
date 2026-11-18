#include "agenticpp/memory_store.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace agenticpp {

MemoryStore::MemoryStore() : MemoryStore(Vectorizer{}, 1024) {}

MemoryStore::MemoryStore(std::size_t reserve) : MemoryStore(Vectorizer{}, reserve) {}

MemoryStore::MemoryStore(const Vectorizer& vectorizer, std::size_t reserve)
    : vectorizer_(vectorizer) {
    records_.reserve(reserve);
    ids_.reserve(reserve);
    row_by_id_.reserve(reserve);
    vectors_.reserve(reserve * Vectorizer::kDim);
}

void MemoryStore::upsert(std::uint64_t id, std::string_view text, FixRecord record) {
    record.template_text = std::string(text);

    std::vector<float> vector(Vectorizer::kDim, 0.0f);
    vectorizer_.embed(text, std::span<float>{vector});

    const auto existing = row_by_id_.find(id);
    if (existing != row_by_id_.end()) {
        const std::size_t row = existing->second;
        std::copy(vector.begin(), vector.end(),
                  vectors_.begin() + static_cast<std::ptrdiff_t>(row) * Vectorizer::kDim);
        records_.insert_or_assign(id, std::move(record));
        return;
    }

    const std::size_t row = ids_.size();
    ids_.push_back(id);
    row_by_id_.emplace(id, row);
    vectors_.insert(vectors_.end(), vector.begin(), vector.end());
    records_.emplace(id, std::move(record));
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
    if (k == 0 || ids_.empty()) {
        return hits;
    }

    std::vector<float> query_vector(Vectorizer::kDim, 0.0f);
    vectorizer_.embed(query, std::span<float>{query_vector});

    float query_norm_squared = 0.0f;
    for (const float value : query_vector) {
        query_norm_squared += value * value;
    }
    if (query_norm_squared <= 0.0f) {
        return hits;
    }

    hits.reserve(ids_.size());
    for (std::size_t row = 0; row < ids_.size(); ++row) {
        const auto record = records_.find(ids_[row]);
        if (record == records_.end()) {
            continue;
        }

        const float* vector =
            vectors_.data() + static_cast<std::ptrdiff_t>(row) * Vectorizer::kDim;
        float score = 0.0f;
        for (std::size_t dimension = 0; dimension < Vectorizer::kDim; ++dimension) {
            score += query_vector[dimension] * vector[dimension];
        }
        hits.push_back(Hit{ids_[row], score, &record->second});
    }

    const std::size_t limit = std::min(k, hits.size());
    std::partial_sort(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(limit), hits.end(),
                      [](const Hit& left, const Hit& right) { return left.score > right.score; });
    hits.resize(limit);
    return hits;
}

std::size_t MemoryStore::size() const noexcept {
    return records_.size();
}

}  // namespace agenticpp
