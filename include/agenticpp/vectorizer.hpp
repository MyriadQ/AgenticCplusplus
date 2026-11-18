#pragma once

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace agenticpp {

// Hashed n-gram vectorizer. No neural model, no external dependency, deterministic.
class Vectorizer {
public:
    static constexpr std::size_t kDim = 512;

    // out.size() must be kDim. The result is L2-normalised; empty/whitespace
    // input produces a finite zero vector.
    void embed(std::string_view text, std::span<float> out) const;
    std::vector<float> embed(std::string_view text) const;
};

}  // namespace agenticpp
