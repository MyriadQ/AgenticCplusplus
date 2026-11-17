#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "agenticpp/template_parser.hpp"

namespace {

bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

}  // namespace

TEST_CASE("TemplateParser: timestamp and IP variants share an id") {
    const auto first =
        agenticpp::parse_template("2026-01-01T00:00:00Z ERROR bind 10.0.0.1:8080 failed");
    const auto second =
        agenticpp::parse_template("2025-12-31T23:59:59Z ERROR bind 192.168.1.2:9090 failed");

    REQUIRE(first.template_id == second.template_id);
    REQUIRE(contains(first.template_text, "<TS>"));
    REQUIRE(contains(first.template_text, "<IP>"));
    REQUIRE(contains(first.template_text, "<NUM>"));
    REQUIRE_FALSE(contains(first.template_text, "10.0.0.1"));
    REQUIRE_FALSE(contains(first.template_text, "8080"));
}

TEST_CASE("TemplateParser: different message text yields a different id") {
    const auto first = agenticpp::parse_template("ERROR bind 10.0.0.1:8080 failed");
    const auto second = agenticpp::parse_template("ERROR bind 10.0.0.1:8080 timeout");

    REQUIRE(first.template_id != second.template_id);
}

TEST_CASE("TemplateParser: captures params in order") {
    const auto parsed =
        agenticpp::parse_template("2026-01-01T00:00:00Z bind 10.0.0.1:8080");

    REQUIRE(parsed.params.size() == 3);
    REQUIRE(parsed.params[0] == "2026-01-01T00:00:00Z");
    REQUIRE(parsed.params[1] == "10.0.0.1");
    REQUIRE(parsed.params[2] == "8080");
}

TEST_CASE("TemplateParser: parse is idempotent on masked text") {
    const auto once =
        agenticpp::parse_template("2026-01-01T00:00:00Z ERROR bind 10.0.0.1:8080 failed");
    const auto twice = agenticpp::parse_template(once.template_text);

    REQUIRE(twice.template_text == once.template_text);
    REQUIRE(twice.template_id == once.template_id);
}

TEST_CASE("TemplateParser: all-digit line is safe") {
    const auto parsed = agenticpp::parse_template("1234567890");

    REQUIRE(parsed.template_text == "<NUM>");
    REQUIRE(parsed.params.size() == 1);
    REQUIRE(parsed.params[0] == "1234567890");
    REQUIRE(parsed.template_id != 0);
}

TEST_CASE("TemplateParser: hashes full text not the truncated display") {
    const std::string prefix(250, 'x');

    const auto first = agenticpp::parse_template(prefix + " alpha");
    const auto second = agenticpp::parse_template(prefix + " beta");

    REQUIRE(first.template_text.size() > 200);
    REQUIRE(first.template_id != second.template_id);
}

TEST_CASE("TemplateParser: masks UUID, IPv6, path, email, quote, hex, and number") {
    const auto parsed = agenticpp::parse_template(
        R"(user "alice" uuid 550e8400-e29b-41d4-a716-446655440000 ip 2001:db8::1 path /var/log/victim.log email a.b+c@example.com hex 0xDEADBEEF code 42)");

    REQUIRE(contains(parsed.template_text, "<STR>"));
    REQUIRE(contains(parsed.template_text, "<UUID>"));
    REQUIRE(contains(parsed.template_text, "<IP>"));
    REQUIRE(contains(parsed.template_text, "<PATH>"));
    REQUIRE(contains(parsed.template_text, "<EMAIL>"));
    REQUIRE(contains(parsed.template_text, "<HEX>"));
    REQUIRE(contains(parsed.template_text, "<NUM>"));

    REQUIRE(parsed.params.size() == 7);
}

TEST_CASE("TemplateParser: masks syslog timestamps") {
    const auto parsed = agenticpp::parse_template("Jan  2 03:04:05 host app: failed");

    REQUIRE(parsed.template_text.starts_with("<TS>"));
    REQUIRE_FALSE(contains(parsed.template_text, "03:04:05"));
}
