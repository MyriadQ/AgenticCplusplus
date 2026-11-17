#include "agenticpp/template_parser.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace agenticpp {
namespace {

struct Mask {
    std::size_t length;
    std::string_view token;
};

bool is_digit(char c) noexcept {
    return std::isdigit(static_cast<unsigned char>(c)) != 0;
}

bool is_hex(char c) noexcept {
    return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

bool is_alpha(char c) noexcept {
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
}

bool is_word(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_path_char(char c) noexcept {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '.' || c == '_' ||
           c == '-' || c == '/' || c == '~' || c == '+' || c == '%' || c == '@';
}

bool has_digits(std::string_view text, std::size_t pos, std::size_t count) noexcept {
    if (pos + count > text.size()) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (!is_digit(text[pos + i])) {
            return false;
        }
    }
    return true;
}

bool boundary_before_word(std::string_view text, std::size_t pos) noexcept {
    return pos == 0 || !is_word(text[pos - 1]);
}

bool boundary_after_word(std::string_view text, std::size_t pos) noexcept {
    return pos >= text.size() || !is_word(text[pos]);
}

bool boundary_before_ip(std::string_view text, std::size_t pos) noexcept {
    return pos == 0 || (!is_word(text[pos - 1]) && text[pos - 1] != '.' && text[pos - 1] != ':');
}

bool boundary_after_ipv4(std::string_view text, std::size_t pos) noexcept {
    // A colon is allowed here because it usually introduces a port.
    return pos >= text.size() || (!is_word(text[pos]) && text[pos] != '.');
}

bool boundary_after_ipv6(std::string_view text, std::size_t pos) noexcept {
    return pos >= text.size() || (!is_word(text[pos]) && text[pos] != '.' && text[pos] != ':');
}

std::size_t match_iso_timestamp(std::string_view line, std::size_t pos) noexcept {
    if (pos + 19 > line.size()) {
        return 0;
    }
    if (!has_digits(line, pos, 4) || line[pos + 4] != '-' || !has_digits(line, pos + 5, 2) ||
        line[pos + 7] != '-' || !has_digits(line, pos + 8, 2)) {
        return 0;
    }
    if (line[pos + 10] != 'T' && line[pos + 10] != ' ') {
        return 0;
    }
    if (!has_digits(line, pos + 11, 2) || line[pos + 13] != ':' ||
        !has_digits(line, pos + 14, 2) || line[pos + 16] != ':' ||
        !has_digits(line, pos + 17, 2)) {
        return 0;
    }

    std::size_t end = pos + 19;
    if (end < line.size() && line[end] == '.') {
        std::size_t fraction = end + 1;
        if (fraction >= line.size() || !is_digit(line[fraction])) {
            return 0;
        }
        while (fraction < line.size() && is_digit(line[fraction])) {
            ++fraction;
        }
        end = fraction;
    }

    if (end < line.size()) {
        if (line[end] == 'Z' || line[end] == 'z') {
            ++end;
        } else if (line[end] == '+' || line[end] == '-') {
            const std::size_t zone = end + 1;
            if (zone + 5 > line.size() || !has_digits(line, zone, 2) ||
                line[zone + 2] != ':' || !has_digits(line, zone + 3, 2)) {
                return 0;
            }
            end = zone + 5;
        }
    }

    if (!boundary_before_word(line, pos) || !boundary_after_word(line, end)) {
        return 0;
    }
    return end - pos;
}

std::size_t match_syslog_timestamp(std::string_view line, std::size_t pos) noexcept {
    static constexpr std::array<std::string_view, 12> kMonths{
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };

    if (pos + 3 > line.size()) {
        return 0;
    }
    const std::string_view month = line.substr(pos, 3);
    bool month_ok = false;
    for (const std::string_view candidate : kMonths) {
        if (month == candidate) {
            month_ok = true;
            break;
        }
    }
    if (!month_ok || !boundary_before_word(line, pos)) {
        return 0;
    }

    std::size_t p = pos + 3;
    if (p >= line.size() || line[p] != ' ') {
        return 0;
    }
    while (p < line.size() && line[p] == ' ') {
        ++p;
    }
    if (p >= line.size() || !is_digit(line[p])) {
        return 0;
    }
    ++p;
    if (p < line.size() && is_digit(line[p])) {
        ++p;
    }
    if (p >= line.size() || line[p] != ' ') {
        return 0;
    }
    ++p;
    if (p + 8 > line.size() || !has_digits(line, p, 2) || line[p + 2] != ':' ||
        !has_digits(line, p + 3, 2) || line[p + 5] != ':' || !has_digits(line, p + 6, 2)) {
        return 0;
    }

    const std::size_t end = p + 8;
    if (!boundary_after_word(line, end)) {
        return 0;
    }
    return end - pos;
}

std::size_t match_timestamp(std::string_view line, std::size_t pos) noexcept {
    if (const std::size_t iso = match_iso_timestamp(line, pos); iso != 0) {
        return iso;
    }
    return match_syslog_timestamp(line, pos);
}

std::size_t match_uuid(std::string_view line, std::size_t pos) noexcept {
    constexpr std::size_t kUuidLength = 36;
    if (pos + kUuidLength > line.size() || !boundary_before_word(line, pos) ||
        !boundary_after_word(line, pos + kUuidLength)) {
        return 0;
    }

    for (std::size_t i = 0; i < kUuidLength; ++i) {
        const char c = line[pos + i];
        const bool hyphen_position = i == 8 || i == 13 || i == 18 || i == 23;
        if (hyphen_position) {
            if (c != '-') {
                return 0;
            }
        } else if (!is_hex(c)) {
            return 0;
        }
    }
    return kUuidLength;
}

bool parse_octet(std::string_view line, std::size_t pos, std::size_t& length) noexcept {
    length = 0;
    int value = 0;
    while (length < 3 && pos + length < line.size() && is_digit(line[pos + length])) {
        value = value * 10 + (line[pos + length] - '0');
        ++length;
    }
    return length != 0 && value <= 255;
}

std::size_t match_ipv4(std::string_view line, std::size_t pos) noexcept {
    if (!boundary_before_ip(line, pos)) {
        return 0;
    }

    std::size_t p = pos;
    for (int octet = 0; octet < 4; ++octet) {
        std::size_t length = 0;
        if (!parse_octet(line, p, length)) {
            return 0;
        }
        p += length;
        if (octet != 3) {
            if (p >= line.size() || line[p] != '.') {
                return 0;
            }
            ++p;
        }
    }

    if (!boundary_after_ipv4(line, p)) {
        return 0;
    }
    return p - pos;
}

std::size_t match_ipv6(std::string_view line, std::size_t pos) noexcept {
    if (pos >= line.size() || !boundary_before_ip(line, pos)) {
        return 0;
    }

    std::size_t p = pos;
    std::size_t groups = 0;
    std::size_t colons = 0;
    bool compressed = false;
    bool last_was_colon = false;

    if (line[p] == ':') {
        if (p + 1 >= line.size() || line[p + 1] != ':') {
            return 0;
        }
        compressed = true;
        colons = 2;
        p += 2;
        last_was_colon = true;
    }

    while (p < line.size()) {
        if (line[p] == ':') {
            if (last_was_colon) {
                if (compressed) {
                    return 0;
                }
                compressed = true;
            }
            ++colons;
            ++p;
            last_was_colon = true;
            continue;
        }

        if (!is_hex(line[p])) {
            break;
        }

        const std::size_t start = p;
        while (p < line.size() && is_hex(line[p]) && p - start < 5) {
            ++p;
        }
        const std::size_t length = p - start;
        if (length == 0 || length > 4) {
            return 0;
        }
        ++groups;
        last_was_colon = false;
    }

    if (colons < 2) {
        return 0;
    }
    if (!compressed && groups != 8) {
        return 0;
    }
    if (compressed && (groups == 0 || groups > 7)) {
        return 0;
    }
    if (!boundary_after_ipv6(line, p)) {
        return 0;
    }
    return p - pos;
}

std::size_t match_hex(std::string_view line, std::size_t pos) noexcept {
    if (!boundary_before_word(line, pos)) {
        return 0;
    }

    if (pos + 2 <= line.size() && line[pos] == '0' &&
        (line[pos + 1] == 'x' || line[pos + 1] == 'X')) {
        std::size_t p = pos + 2;
        if (p >= line.size() || !is_hex(line[p])) {
            return 0;
        }
        while (p < line.size() && is_hex(line[p])) {
            ++p;
        }
        if (!boundary_after_word(line, p)) {
            return 0;
        }
        return p - pos;
    }

    std::size_t p = pos;
    bool has_alpha = false;
    while (p < line.size() && is_hex(line[p])) {
        if (is_alpha(line[p])) {
            has_alpha = true;
        }
        ++p;
    }
    const std::size_t length = p - pos;
    if (length < 8 || !has_alpha || !boundary_after_word(line, p)) {
        return 0;
    }
    return length;
}

std::size_t match_quoted(std::string_view line, std::size_t pos) noexcept {
    if (pos >= line.size()) {
        return 0;
    }
    const char quote = line[pos];
    if (quote != '"' && quote != '\'') {
        return 0;
    }

    std::size_t p = pos + 1;
    while (p < line.size()) {
        if (line[p] == '\\' && p + 1 < line.size()) {
            p += 2;
            continue;
        }
        if (line[p] == quote) {
            return p - pos + 1;
        }
        ++p;
    }
    return 0;
}

std::size_t match_path(std::string_view line, std::size_t pos) noexcept {
    if (line[pos] != '/') {
        return 0;
    }
    if (pos > 0 && is_path_char(line[pos - 1])) {
        return 0;
    }

    std::size_t p = pos + 1;
    while (p < line.size() && is_path_char(line[p])) {
        ++p;
    }
    if (p - pos < 2) {
        return 0;
    }
    return p - pos;
}

std::size_t match_email(std::string_view line, std::size_t pos) noexcept {
    if (!boundary_before_word(line, pos)) {
        return 0;
    }

    std::size_t p = pos;
    while (p < line.size() && (std::isalnum(static_cast<unsigned char>(line[p])) != 0 ||
                                line[p] == '.' || line[p] == '_' || line[p] == '%' ||
                                line[p] == '+' || line[p] == '-')) {
        ++p;
    }
    if (p == pos || p >= line.size() || line[p] != '@') {
        return 0;
    }

    const std::size_t local_end = p;
    ++p;
    const std::size_t domain_start = p;
    while (p < line.size() && (std::isalnum(static_cast<unsigned char>(line[p])) != 0 ||
                                line[p] == '-' || line[p] == '.')) {
        ++p;
    }
    if (p == domain_start) {
        return 0;
    }

    const std::size_t dot = line.rfind('.', p - 1);
    if (dot == std::string_view::npos || dot < domain_start || dot + 1 >= p) {
        return 0;
    }
    if (p - (dot + 1) < 2) {
        return 0;
    }
    for (std::size_t i = dot + 1; i < p; ++i) {
        if (!is_alpha(line[i])) {
            return 0;
        }
    }
    if (!boundary_after_word(line, p)) {
        return 0;
    }

    // Keep the local part non-empty and avoid treating a bare domain as an email.
    return local_end == pos ? 0 : p - pos;
}

std::size_t match_number(std::string_view line, std::size_t pos) noexcept {
    if (pos >= line.size()) {
        return 0;
    }
    if (pos > 0 && (is_word(line[pos - 1]) || line[pos - 1] == '.')) {
        return 0;
    }

    std::size_t p = pos;
    if ((line[p] == '+' || line[p] == '-') && p + 1 < line.size() && is_digit(line[p + 1])) {
        ++p;
    }

    bool has_digits = false;
    while (p < line.size() && is_digit(line[p])) {
        has_digits = true;
        ++p;
    }
    if (p < line.size() && line[p] == '.') {
        std::size_t fraction = p + 1;
        while (fraction < line.size() && is_digit(line[fraction])) {
            ++fraction;
        }
        if (fraction > p + 1) {
            has_digits = true;
            p = fraction;
        }
    }
    if (!has_digits) {
        return 0;
    }

    if (p < line.size() && (line[p] == 'e' || line[p] == 'E')) {
        std::size_t exponent = p + 1;
        if (exponent < line.size() && (line[exponent] == '+' || line[exponent] == '-')) {
            ++exponent;
        }
        const std::size_t exponent_start = exponent;
        while (exponent < line.size() && is_digit(line[exponent])) {
            ++exponent;
        }
        if (exponent > exponent_start) {
            p = exponent;
        }
    }

    if (p < line.size() && (is_word(line[p]) || line[p] == '.')) {
        return 0;
    }
    return p - pos;
}

std::optional<Mask> match_mask(std::string_view line, std::size_t pos) noexcept {
    if (const std::size_t length = match_timestamp(line, pos); length != 0) {
        return Mask{length, "<TS>"};
    }
    if (const std::size_t length = match_uuid(line, pos); length != 0) {
        return Mask{length, "<UUID>"};
    }
    if (const std::size_t length = match_ipv4(line, pos); length != 0) {
        return Mask{length, "<IP>"};
    }
    if (const std::size_t length = match_ipv6(line, pos); length != 0) {
        return Mask{length, "<IP>"};
    }
    if (const std::size_t length = match_hex(line, pos); length != 0) {
        return Mask{length, "<HEX>"};
    }
    if (const std::size_t length = match_quoted(line, pos); length != 0) {
        return Mask{length, "<STR>"};
    }
    if (const std::size_t length = match_path(line, pos); length != 0) {
        return Mask{length, "<PATH>"};
    }
    if (const std::size_t length = match_email(line, pos); length != 0) {
        return Mask{length, "<EMAIL>"};
    }
    if (const std::size_t length = match_number(line, pos); length != 0) {
        return Mask{length, "<NUM>"};
    }
    return std::nullopt;
}

}  // namespace

std::uint64_t fnv1a64(std::string_view text) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

ParsedLine parse_template(std::string_view line) {
    ParsedLine parsed;
    parsed.template_text.reserve(line.size());

    std::size_t pos = 0;
    while (pos < line.size()) {
        const std::optional<Mask> mask = match_mask(line, pos);
        if (mask.has_value()) {
            parsed.template_text.append(mask->token);
            parsed.params.emplace_back(line.substr(pos, mask->length));
            pos += mask->length;
        } else {
            parsed.template_text.push_back(line[pos]);
            ++pos;
        }
    }

    parsed.template_id = fnv1a64(parsed.template_text);
    return parsed;
}

}  // namespace agenticpp
