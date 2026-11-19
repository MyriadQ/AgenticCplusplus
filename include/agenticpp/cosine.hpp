#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace agenticpp {

struct CosineHit {
    std::uint64_t template_id = 0;
    float score = 0.0f;
};

// Both implementations expect L2-normalised query and matrix rows. They return
// the same top-k ids; ties are resolved by template id for determinism.
std::size_t cosine_topk_scalar(std::span<const float> query, std::span<const float> matrix,
                        std::span<const std::uint64_t> ids, std::size_t count,
                        std::size_t dim, std::span<CosineHit> out);

std::size_t cosine_topk_avx2(std::span<const float> query, std::span<const float> matrix,
                      std::span<const std::uint64_t> ids, std::size_t count,
                      std::size_t dim, std::span<CosineHit> out);

// True when the AVX2/FMA implementation is available and selected at runtime.
bool cosine_avx2_available() noexcept;

// Scalar, AVX2, or the best available implementation.
std::size_t cosine_topk(std::span<const float> query, std::span<const float> matrix,
                 std::span<const std::uint64_t> ids, std::size_t count,
                 std::size_t dim, std::span<CosineHit> out);

}  // namespace agenticpp
