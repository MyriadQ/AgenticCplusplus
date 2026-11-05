#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <unistd.h>

#include <type_traits>
#include <utility>

#include "agenticpp/unique_fd.hpp"

namespace {

int open_dev_null() {
    const int fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    return fd;
}

bool fd_is_open(int fd) {
    return fd >= 0 && ::fcntl(fd, F_GETFD) != -1;
}

}  // namespace

TEST_CASE("UniqueFd: default is invalid") {
    agenticpp::UniqueFd fd;
    REQUIRE(fd.get() == -1);
    REQUIRE_FALSE(static_cast<bool>(fd));
}

TEST_CASE("UniqueFd: owns and closes a descriptor") {
    const int raw = open_dev_null();
    {
        agenticpp::UniqueFd fd{raw};
        REQUIRE(fd.get() == raw);
        REQUIRE(static_cast<bool>(fd));
        REQUIRE(fd_is_open(raw));
    }
    REQUIRE_FALSE(fd_is_open(raw));
}

TEST_CASE("UniqueFd: is move-only") {
    using agenticpp::UniqueFd;
    static_assert(!std::is_copy_constructible_v<UniqueFd>);
    static_assert(!std::is_copy_assignable_v<UniqueFd>);
    static_assert(std::is_nothrow_move_constructible_v<UniqueFd>);
    static_assert(std::is_nothrow_move_assignable_v<UniqueFd>);
    SUCCEED("copy is deleted; move operations are noexcept");
}

TEST_CASE("UniqueFd: move transfers ownership") {
    SECTION("move construction") {
        const int raw = open_dev_null();
        {
            agenticpp::UniqueFd source{raw};
            agenticpp::UniqueFd target{std::move(source)};
            REQUIRE(source.get() == -1);
            REQUIRE_FALSE(static_cast<bool>(source));
            REQUIRE(target.get() == raw);
            REQUIRE(fd_is_open(raw));
        }
        REQUIRE_FALSE(fd_is_open(raw));
    }

    SECTION("move assignment closes the old target") {
        const int first = open_dev_null();
        const int second = open_dev_null();
        {
            agenticpp::UniqueFd source{first};
            agenticpp::UniqueFd target{second};
            target = std::move(source);
            REQUIRE(source.get() == -1);
            REQUIRE(target.get() == first);
            REQUIRE_FALSE(fd_is_open(second));
            REQUIRE(fd_is_open(first));
        }
        REQUIRE_FALSE(fd_is_open(first));
    }
}

TEST_CASE("UniqueFd: reset and release") {
    SECTION("reset closes the old fd and adopts the new one") {
        const int first = open_dev_null();
        const int second = open_dev_null();
        {
            agenticpp::UniqueFd fd{first};
            fd.reset(second);
            REQUIRE(fd.get() == second);
            REQUIRE_FALSE(fd_is_open(first));
            REQUIRE(fd_is_open(second));
        }
        REQUIRE_FALSE(fd_is_open(second));
    }

    SECTION("release gives up ownership without closing") {
        const int raw = open_dev_null();
        {
            agenticpp::UniqueFd fd{raw};
            const int released = fd.release();
            REQUIRE(released == raw);
            REQUIRE_FALSE(static_cast<bool>(fd));
            REQUIRE(fd_is_open(raw));
            ::close(released);
        }
        REQUIRE_FALSE(fd_is_open(raw));
    }
}

TEST_CASE("UniqueFd: self-move-assignment is harmless") {
    const int raw = open_dev_null();
    {
        agenticpp::UniqueFd fd{raw};
        agenticpp::UniqueFd& alias = fd;
        fd = std::move(alias);
        REQUIRE(fd.get() == raw);
        REQUIRE(fd_is_open(raw));
    }
    REQUIRE_FALSE(fd_is_open(raw));
}