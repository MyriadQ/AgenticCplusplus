// Because this is a POSIX daemon, it owns file descriptor
// UniqueFd guards the descriptor lifetime - preventing fd leaks
// and double-close - not heap memory

#pragma once //prevent stacked #include
#include <unistd.h>

namespace agenticpp{
// Owns a file descriptor. Move-only: exactly one UniqueFd owns a given fd.
class UniqueFd{
    public:
    // set Default constructor, because a in-class member is initialize
    // int = fd_ = -1 -> constexpr UniqueFd() noexcept : fd_(-1) {}
    UniqueFd() noexcept = default;

    // Converting constructor - marked with explicit specifier to prevent
    // implicit conversion and copy-initialization
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}

    // Destructor
    ~UniqueFd() noexcept{
        if(fd_ >= 0){
            ::close(fd_);
        }
    }

    // Delete Copy constructor and Copy assignment
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    // Move constructor
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}

    // Move assignment
    UniqueFd& operator=(UniqueFd&& other) noexcept{
        if(this != &other){
            reset(other.release());
        }
        return *this;
    }

    int get() const noexcept{return fd_;}
    explicit operator bool() const noexcept {return fd_ >= 0;}

    // Give up ownership without closing 
    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    //close the currently owned fd(if exist) and adopt fd
    void reset(int fd = -1) noexcept{
        if(fd == fd_){
            return;
        }
        if(fd_ >= 0){
            ::close(fd_);
        }
        fd_ = fd;
    }
    private:
    int fd_ = -1;
};

} // named namespace -> external linkage



