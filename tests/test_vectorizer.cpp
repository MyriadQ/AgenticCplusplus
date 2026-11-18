#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <span>
#include <string_view>
#include <vector>

#include "agenticpp/vectorizer.hpp"

namespace {

float dot(std::span<const float> left, std::span<const float> right) {
    float result = 0.0f;
    for (std::size_t i = 0; i < left.size(); ++i) {
        result += left[i] * right[i];
    }
    return result;
}

}  // namespace

TEST_CASE("Vectorizer: self cosine is one") {
    const agenticpp::Vectorizer vectorizer;
    const std::vector<float> first = vectorizer.embed("bind: <IP>:<NUM> failed");
    const std::vector<float> second = vectorizer.embed("bind: <IP>:<NUM> failed");

    REQUIRE(first.size() == agenticpp::Vectorizer::kDim);
    REQUIRE(dot(first, second) == Catch::Approx(1.0f).margin(1e-6f));
}

TEST_CASE("Vectorizer: case and whitespace variants are near one") {
    const agenticpp::Vectorizer vectorizer;
    const std::vector<float> first = vectorizer.embed("Bind: <IP>:<NUM> Failed");
    const std::vector<float> second = vectorizer.embed("bind:   <ip>:<num> failed");

    REQUIRE(dot(first, second) >= 0.99f);
}

TEST_CASE("Vectorizer: unrelated messages are below 0.5") {
    const agenticpp::Vectorizer vectorizer;
    const std::vector<float> first = vectorizer.embed("bind: address already in use");
    const std::vector<float> second = vectorizer.embed("parse config: unexpected token");

    REQUIRE(dot(first, second) < 0.5f);
}

TEST_CASE("Vectorizer: empty input is zero and finite") {
    const agenticpp::Vectorizer vectorizer;

    for (const std::string_view text : {"", "   "}) {
        const std::vector<float> vector = vectorizer.embed(text);
        REQUIRE(vector.size() == agenticpp::Vectorizer::kDim);

        bool all_zero = true;
        for (const float value : vector) {
            REQUIRE(std::isfinite(value));
            if (value != 0.0f) {
                all_zero = false;
            }
        }
        REQUIRE(all_zero);
    }
}

TEST_CASE("Vectorizer: output is deterministic and normalised") {
    const agenticpp::Vectorizer vectorizer;
    const std::vector<float> first = vectorizer.embed("template <TS> service <STR> failed");
    const std::vector<float> second = vectorizer.embed("template <TS> service <STR> failed");

    REQUIRE(first == second);

    float norm_squared = 0.0f;
    for (const float value : first) {
        norm_squared += value * value;
    }
    REQUIRE(std::sqrt(norm_squared) == Catch::Approx(1.0f).margin(1e-6f));
}
