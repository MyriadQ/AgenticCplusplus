#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string_view>
#include <vector>

#include "agenticpp/cosine.hpp"

namespace {

struct Options {
    std::size_t n = 10000;
    std::size_t dim = 512;
    std::size_t k = 3;
    std::size_t iterations = 20;
};

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

double percentile_ms(std::vector<double> samples, double percentile) {
    if (samples.empty()) {
        return 0.0;
    }
    std::sort(samples.begin(), samples.end());
    const std::size_t index = static_cast<std::size_t>(
        std::min<double>(samples.size() - 1, percentile * (samples.size() - 1)));
    return samples[index];
}

template <class Function>
std::vector<double> measure(std::size_t iterations, Function function) {
    std::vector<double> samples;
    samples.reserve(iterations);
    for (std::size_t i = 0; i < iterations; ++i) {
        const auto started = std::chrono::steady_clock::now();
        function();
        const auto finished = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(finished - started).count());
    }
    return samples;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--n" && i + 1 < argc) {
            options.n = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--dim" && i + 1 < argc) {
            options.dim = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--k" && i + 1 < argc) {
            options.k = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (arg == "--iters" && i + 1 < argc) {
            options.iterations =
                static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        }
    }
    if (options.n == 0 || options.dim == 0 || options.k == 0 || options.iterations == 0) {
        std::fprintf(stderr, "bench_search: n, dim, k, iters must be positive\n");
        return 2;
    }

    std::mt19937 rng(12345);
    std::normal_distribution<float> distribution(0.0f, 1.0f);
    std::vector<float> matrix(options.n * options.dim);
    for (float& value : matrix) {
        value = distribution(rng);
    }
    for (std::size_t row = 0; row < options.n; ++row) {
        std::vector<float> values(options.dim);
        std::copy(matrix.begin() + static_cast<std::ptrdiff_t>(row * options.dim),
                  matrix.begin() + static_cast<std::ptrdiff_t>((row + 1) * options.dim),
                  values.begin());
        normalise(values);
        std::copy(values.begin(), values.end(),
                  matrix.begin() + static_cast<std::ptrdiff_t>(row * options.dim));
    }

    std::vector<float> query(options.dim);
    for (float& value : query) {
        value = distribution(rng);
    }
    normalise(query);

    std::vector<std::uint64_t> ids(options.n);
    for (std::size_t i = 0; i < options.n; ++i) {
        ids[i] = i + 1;
    }

    const std::size_t k = std::min(options.k, options.n);
    std::vector<agenticpp::CosineHit> hits(k);

    const std::vector<double> scalar_ms = measure(options.iterations, [&] {
        (void)agenticpp::cosine_topk_scalar(query, matrix, ids, options.n, options.dim, hits);
    });
    const std::vector<double> dispatched_ms = measure(options.iterations, [&] {
        (void)agenticpp::cosine_topk(query, matrix, ids, options.n, options.dim, hits);
    });

    std::printf("n=%zu dim=%zu k=%zu iters=%zu avx2=%s\n", options.n, options.dim, k,
                options.iterations, agenticpp::cosine_avx2_available() ? "yes" : "no");
    std::printf("scalar p50=%.3fms p99=%.3fms\n", percentile_ms(scalar_ms, 0.50),
                percentile_ms(scalar_ms, 0.99));
    std::printf("dispatch p50=%.3fms p99=%.3fms\n", percentile_ms(dispatched_ms, 0.50),
                percentile_ms(dispatched_ms, 0.99));
    return 0;
}
