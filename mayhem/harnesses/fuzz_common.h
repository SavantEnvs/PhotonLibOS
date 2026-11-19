// Shared setup for the PhotonLibOS fuzz harnesses (mayhem/ layer, not upstream code).
#pragma once
#include <photon/photon.h>
#include <photon/common/alog.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <sys/uio.h>
#include <photon/common/stream.h>

// One photon vCPU for the whole process (epoll only; no io engine is needed because every
// harness feeds bytes through an in-memory stream). Logging is silenced: it is global state
// that does not influence parsing, and writing every rejected input to stderr is pure noise.
static inline void fuzz_photon_init() {
    static bool done = false;
    if (done) return;
    done = true;
    set_log_output_level(ALOG_FATAL);
    if (photon::init(photon::INIT_EVENT_EPOLL, photon::INIT_IO_NONE) != 0) __builtin_trap();
}

// A peer connection for read()-based protocol readers: reads consume the fuzz input and return 0 (EOF) at its end, writes
// (the responses) are discarded. (StringSocketStream::read peeks without consuming, so a
// read()-based reader like Skeleton::serve would loop on it forever.)
class InputStream : public IStream {
    const char* p; size_t n;
public:
    InputStream(const uint8_t* d, size_t s) : p((const char*)d), n(s) {}
    int close() override { n = 0; return 0; }
    ssize_t read(void* buf, size_t count) override {
        if (count > n) count = n;
        memcpy(buf, p, count);
        p += count; n -= count;
        return count;
    }
    ssize_t readv(const struct iovec* iov, int iovcnt) override {
        ssize_t s = 0;
        for (int i = 0; i < iovcnt; i++) {
            ssize_t r = read(iov[i].iov_base, iov[i].iov_len);
            s += r;
            if ((size_t)r < iov[i].iov_len) break;
        }
        return s;
    }
    ssize_t write(const void*, size_t count) override { return count; }
    ssize_t writev(const struct iovec* iov, int iovcnt) override {
        ssize_t s = 0;
        for (int i = 0; i < iovcnt; i++) s += iov[i].iov_len;
        return s;
    }
};

