#include "agenticpp/cosine.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "cosine_detail.hpp"

namespace agenticpp {

std::size_t cosine_topk_scalar(std::span<const float> query, std::span<const float> matrix,
                               std::span<const std::uint64_t> ids, std::size_t count,
                               std::size_t dim, std::span<CosineHit> out) {
    detail::init_topk(out);
    if (out.empty() || count == 0 || dim == 0) {
        return 0;
    }
    if (ids.size() < count || matrix.size() < count * dim || query.size() < dim) {
        return 0;
    }

    for (std::size_t row = 0; row < count; ++row) {
        const float* values = matrix.data() + row * dim;
        float score = 0.0f;
        for (std::size_t i = 0; i < dim; ++i) {
            score += query[i] * values[i];
        }
        detail::consider_topk(out, ids[row], score);
    }
    return detail::used_topk(out);
}

#if !AGENTICPP_HAVE_AVX2
std::size_t cosine_topk_avx2(std::span<const float> query, std::span<const float> matrix,
                             std::span<const std::uint64_t> ids, std::size_t count,
                             std::size_t dim, std::span<CosineHit> out) {
    return cosine_topk_scalar(query, matrix, ids, count, dim, out);
}
#endif

bool cosine_avx2_available() noexcept {
#if AGENTICPP_HAVE_AVX2
    static const bool supported = [] {
        __builtin_cpu_init();
        return __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
    }();
    return supported;
#else
    return false;
#endif
}

std::size_t cosine_topk(std::span<const float> query, std::span<const float> matrix,
                        std::span<const std::uint64_t> ids, std::size_t count,
                        std::size_t dim, std::span<CosineHit> out) {
    if (cosine_avx2_available()) {
        return cosine_topk_avx2(query, matrix, ids, count, dim, out);
    }
    return cosine_topk_scalar(query, matrix, ids, count, dim, out);
}

}  // namespace agenticpp
