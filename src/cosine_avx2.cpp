#include "agenticpp/cosine.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

#include <immintrin.h>

#include "cosine_detail.hpp"

namespace agenticpp {
namespace {

float horizontal_sum(__m256 values) noexcept {
    __m128 low = _mm256_castps256_ps128(values);
    __m128 high = _mm256_extractf128_ps(values, 1);
    __m128 sum = _mm_add_ps(low, high);
    sum = _mm_hadd_ps(sum, sum);
    sum = _mm_hadd_ps(sum, sum);
    return _mm_cvtss_f32(sum);
}

}  // namespace

std::size_t cosine_topk_avx2(std::span<const float> query, std::span<const float> matrix,
                             std::span<const std::uint64_t> ids, std::size_t count,
                             std::size_t dim, std::span<CosineHit> out) {
    detail::init_topk(out);
    if (out.empty() || count == 0 || dim == 0) {
        return 0;
    }
    if (ids.size() < count || matrix.size() < count * dim || query.size() < dim) {
        return 0;
    }

    const std::size_t vector_end = dim - (dim % 8);
    for (std::size_t row = 0; row < count; ++row) {
        const float* values = matrix.data() + row * dim;
        __m256 accumulator = _mm256_setzero_ps();
        for (std::size_t i = 0; i < vector_end; i += 8) {
            const __m256 q = _mm256_loadu_ps(query.data() + i);
            const __m256 v = _mm256_loadu_ps(values + i);
            accumulator = _mm256_fmadd_ps(q, v, accumulator);
        }
        float score = horizontal_sum(accumulator);
        for (std::size_t i = vector_end; i < dim; ++i) {
            score += query[i] * values[i];
        }
        detail::consider_topk(out, ids[row], score);
    }
    return detail::used_topk(out);
}

}  // namespace agenticpp
