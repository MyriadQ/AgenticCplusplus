#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "agenticpp/result.hpp"

TEST_CASE("Result: carries a value") {
    auto result = agenticpp::Result<int>::ok(42);

    REQUIRE(result.has_value());
    REQUIRE(static_cast<bool>(result));
    REQUIRE(result.value() == 42);
    REQUIRE(*result == 42);
}

TEST_CASE("Result: carries an error") {
    auto result = agenticpp::Result<int>::err({7, "bad input"});

    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(static_cast<bool>(result));
    REQUIRE(result.error().code == 7);
    REQUIRE(result.error().message == "bad input");
}

TEST_CASE("Result<void>: success has no value") {
    auto result = agenticpp::Result<void>::ok();

    REQUIRE(result.has_value());
    REQUIRE(static_cast<bool>(result));
}

TEST_CASE("Result<void>: carries an error") {
    auto result = agenticpp::Result<void>::err({9, "void failure"});

    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(static_cast<bool>(result));
    REQUIRE(result.error().code == 9);
    REQUIRE(result.error().message == "void failure");
}

TEST_CASE("Result: move-only value compiles") {
    using ResultPtr = agenticpp::Result<std::unique_ptr<int>>;

    static_assert(!std::is_copy_constructible_v<ResultPtr>);
    static_assert(std::is_move_constructible_v<ResultPtr>);

    auto result = ResultPtr::ok(std::make_unique<int>(7));

    REQUIRE(result.has_value());
    REQUIRE(*result.value() == 7);

    auto moved = std::move(result);
    REQUIRE(moved.has_value());
    REQUIRE(*moved.value() == 7);
}

TEST_CASE("Result: convenience error factory") {
    auto result = agenticpp::Result<std::string>::err(3, "open failed");

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == 3);
    REQUIRE(result.error().message == "open failed");
}
