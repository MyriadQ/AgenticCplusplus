#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "agenticpp/cosine.hpp"

namespace {

void normalise(std::vector<float>& values) {
    float norm_squared = 0.0f;
    for (const float value : values) {
        norm_squared += value * value;
    }
    if (norm_squared <= 0.0f) {
        return;
    }
    const float inverse = 1.0f / std::sqrt(norm_squared);
    for (float& value : values) {
        value *= inverse;
    }
}

std::vector<float> random_vector(std::mt19937& rng, std::size_t dim) {
    std::normal_distribution<float> distribution(0.0f, 1.0f);
    std::vector<float> values(dim);
    for (float& value : values) {
        value = distribution(rng);
    }
    normalise(values);
    return values;
}

}  // namespace

TEST_CASE("Cosine: scalar scores an identical row as one") {
    const std::vector<float> query{0.6f, 0.8f, 0.0f};
    const std::vector<float> matrix{0.6f, 0.8f, 0.0f};
    const std::vector<std::uint64_t> ids{11};
    std::vector<agenticpp::CosineHit> hits(1);

    const std::size_t used = agenticpp::cosine_topk_scalar(query, matrix, ids, 1, 3, hits);
    REQUIRE(used == 1);
    REQUIRE(hits[0].template_id == 11);
    REQUIRE(hits[0].score == Catch::Approx(1.0f).margin(1e-6f));
}

TEST_CASE("Cosine: scalar and AVX2 return identical top-k ids") {
    if (!agenticpp::cosine_avx2_available()) {
        SKIP("AVX2/FMA is not available on this CPU");
    }

    std::mt19937 rng(12345);
    constexpr std::size_t kDim = 512;
    constexpr std::size_t kRows = 37;
    constexpr std::size_t k = 5;

    const std::vector<float> query = random_vector(rng, kDim);
    std::vector<float> matrix(kRows * kDim);
    std::vector<std::uint64_t> ids(kRows);
    for (std::size_t row = 0; row < kRows; ++row) {
        const std::vector<float> values = random_vector(rng, kDim);
        std::copy(values.begin(), values.end(), matrix.begin() + row * kDim);
        ids[row] = 1000 + row;
    }

    std::vector<agenticpp::CosineHit> scalar(k);
    std::vector<agenticpp::CosineHit> avx2(k);
    const std::size_t scalar_used =
        agenticpp::cosine_topk_scalar(query, matrix, ids, kRows, kDim, scalar);
    const std::size_t avx2_used =
        agenticpp::cosine_topk_avx2(query, matrix, ids, kRows, kDim, avx2);

    REQUIRE(scalar_used == avx2_used);
    REQUIRE(scalar_used == k);
    for (std::size_t i = 0; i < k; ++i) {
        REQUIRE(scalar[i].template_id == avx2[i].template_id);
        REQUIRE(scalar[i].score == Catch::Approx(avx2[i].score).margin(1e-5f));
    }
}

TEST_CASE("Cosine: tail dimensions are handled identically") {
    if (!agenticpp::cosine_avx2_available()) {
        SKIP("AVX2/FMA is not available on this CPU");
    }

    std::mt19937 rng(77);
    constexpr std::size_t kDim = 13;
    constexpr std::size_t kRows = 9;
    const std::vector<float> query = random_vector(rng, kDim);
    std::vector<float> matrix(kRows * kDim);
    std::vector<std::uint64_t> ids(kRows);
    for (std::size_t row = 0; row < kRows; ++row) {
        const std::vector<float> values = random_vector(rng, kDim);
        std::copy(values.begin(), values.end(), matrix.begin() + row * kDim);
        ids[row] = 200 + row;
    }

    std::vector<agenticpp::CosineHit> scalar(3);
    std::vector<agenticpp::CosineHit> avx2(3);
    const std::size_t scalar_used =
        agenticpp::cosine_topk_scalar(query, matrix, ids, kRows, kDim, scalar);
    const std::size_t avx2_used =
        agenticpp::cosine_topk_avx2(query, matrix, ids, kRows, kDim, avx2);

    REQUIRE(scalar_used == avx2_used);
    for (std::size_t i = 0; i < scalar_used; ++i) {
        REQUIRE(scalar[i].template_id == avx2[i].template_id);
        REQUIRE(scalar[i].score == Catch::Approx(avx2[i].score).margin(1e-5f));
    }
}

TEST_CASE("Cosine: empty matrix and zero k return no hits") {
    const std::vector<float> query{1.0f, 0.0f};
    std::vector<agenticpp::CosineHit> hits(3);

    REQUIRE(agenticpp::cosine_topk_scalar(query, {}, {}, 0, 2, hits) == 0);
    REQUIRE(agenticpp::cosine_topk(query, {}, {}, 0, 2, hits) == 0);
}

TEST_CASE("Cosine: equal scores break ties by template id") {
    const std::vector<float> query{1.0f, 0.0f};
    const std::vector<float> matrix{
        1.0f, 0.0f,
        1.0f, 0.0f,
    };
    const std::vector<std::uint64_t> ids{9, 4};
    std::vector<agenticpp::CosineHit> hits(2);

    const std::size_t used = agenticpp::cosine_topk_scalar(query, matrix, ids, 2, 2, hits);
    REQUIRE(used == 2);
    REQUIRE(hits[0].template_id == 4);
    REQUIRE(hits[1].template_id == 9);
}
