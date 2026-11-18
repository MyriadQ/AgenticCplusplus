#include "agenticpp/vectorizer.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agenticpp/template_parser.hpp"

namespace agenticpp {
namespace {

char lower(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

std::vector<std::string> tokenize(std::string_view text) {
    std::vector<std::string> tokens;

    std::size_t pos = 0;
    while (pos < text.size()) {
        const unsigned char current = static_cast<unsigned char>(text[pos]);

        if (current == '<') {
            const std::size_t end = text.find('>', pos + 1);
            if (end != std::string_view::npos) {
                std::string token;
                token.reserve(end - pos + 1);
                for (std::size_t i = pos; i <= end; ++i) {
                    token.push_back(lower(text[i]));
                }
                tokens.push_back(std::move(token));
                pos = end + 1;
                continue;
            }
        }

        if (std::isalnum(current) != 0) {
            std::string token;
            while (pos < text.size() &&
                   std::isalnum(static_cast<unsigned char>(text[pos])) != 0) {
                token.push_back(lower(text[pos]));
                ++pos;
            }
            tokens.push_back(std::move(token));
            continue;
        }

        ++pos;
    }

    return tokens;
}

void add_feature(std::span<float> out, std::string_view feature, float weight) {
    if (feature.empty()) {
        return;
    }

    const std::uint64_t hash = fnv1a64(feature);
    const std::size_t index = static_cast<std::size_t>(hash % Vectorizer::kDim);
    const float sign = (hash >> 63) != 0 ? -1.0f : 1.0f;
    out[index] += sign * weight;
}

}  // namespace

void Vectorizer::embed(std::string_view text, std::span<float> out) const {
    if (out.size() != kDim) {
        return;
    }
    std::fill(out.begin(), out.end(), 0.0f);

    const std::vector<std::string> tokens = tokenize(text);

    for (const std::string& token : tokens) {
        add_feature(out, token, 1.0f);
    }
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        const std::string bigram = tokens[i] + " " + tokens[i + 1];
        add_feature(out, bigram, 1.0f);
    }

    // Character grams use the normalised token stream, so case and whitespace
    // variants produce the same features instead of drifting apart.
    std::string normalized;
    for (const std::string& token : tokens) {
        if (!normalized.empty()) {
            normalized.push_back(' ');
        }
        normalized += token;
    }
    for (std::size_t i = 0; i + 2 < normalized.size(); ++i) {
        add_feature(out, std::string_view{normalized.data() + i, 3}, 0.5f);
    }

    float norm_squared = 0.0f;
    for (const float value : out) {
        norm_squared += value * value;
    }
    if (norm_squared <= 0.0f) {
        return;
    }

    const float inverse_norm = 1.0f / std::sqrt(norm_squared);
    for (float& value : out) {
        value *= inverse_norm;
    }
}

std::vector<float> Vectorizer::embed(std::string_view text) const {
    std::vector<float> out(kDim, 0.0f);
    embed(text, std::span<float>{out});
    return out;
}

}  // namespace agenticpp
