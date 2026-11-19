// Unsanitized helper used by executor sandbox tests. It exercises one
// syscall and reports whether the sandbox blocked it.
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <ifaddrs.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "sandbox_probe: missing mode\n");
        return 2;
    }
    const char* mode = argv[1];

    if (std::strcmp(mode, "socket") == 0) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            ::close(fd);
            std::printf("socket allowed\n");
            return 1;
        }
        std::printf("socket errno=%d\n", errno);
        return errno == EPERM ? 0 : 3;
    }

    if (std::strcmp(mode, "interfaces") == 0) {
        ifaddrs* list = nullptr;
        if (::getifaddrs(&list) != 0) {
            std::printf("getifaddrs errno=%d\n", errno);
            return 3;
        }
        int non_loopback = 0;
        for (ifaddrs* item = list; item != nullptr; item = item->ifa_next) {
            if (item->ifa_name == nullptr) {
                continue;
            }
            std::printf("iface=%s\n", item->ifa_name);
            if (std::strcmp(item->ifa_name, "lo") != 0) {
                ++non_loopback;
            }
        }
        ::freeifaddrs(list);
        return non_loopback == 0 ? 0 : 1;
    }

    if (std::strcmp(mode, "unshare") == 0) {
        if (::unshare(CLONE_NEWNS) == 0) {
            std::printf("unshare allowed\n");
            return 1;
        }
        std::printf("unshare errno=%d\n", errno);
        return errno == EPERM ? 0 : 3;
    }

    std::fprintf(stderr, "sandbox_probe: unknown mode %s\n", mode);
    return 2;
}
