#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "agenticpp/cosine.hpp"

namespace agenticpp {
namespace detail {

inline void init_topk(std::span<CosineHit> out) {
    for (CosineHit& hit : out) {
        hit.template_id = std::numeric_limits<std::uint64_t>::max();
        hit.score = -std::numeric_limits<float>::infinity();
    }
}

inline void consider_topk(std::span<CosineHit> out, std::uint64_t id, float score) {
    for (std::size_t i = 0; i < out.size(); ++i) {
        const bool better =
            score > out[i].score ||
            (score == out[i].score && id < out[i].template_id);
        if (!better) {
            continue;
        }
        for (std::size_t j = out.size(); j > i + 1; --j) {
            out[j - 1] = out[j - 2];
        }
        out[i] = CosineHit{id, score};
        return;
    }
}

inline std::size_t used_topk(std::span<const CosineHit> out) {
    std::size_t used = 0;
    for (const CosineHit& hit : out) {
        if (hit.score == -std::numeric_limits<float>::infinity()) {
            break;
        }
        ++used;
    }
    return used;
}

}  // namespace detail
}  // namespace agenticpp
