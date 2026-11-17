#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace agenticpp {

struct ParsedLine {
    std::string template_text;        // full masked text
    std::uint64_t template_id = 0;    // FNV-1a 64 over template_text
    std::vector<std::string> params;  // captured originals, left to right
};

// Masks volatile fields in a fixed order and hashes the full masked text.
ParsedLine parse_template(std::string_view line);

// Exposed so callers can hash other stable text consistently with template IDs.
std::uint64_t fnv1a64(std::string_view text) noexcept;

}  // namespace agenticpp
