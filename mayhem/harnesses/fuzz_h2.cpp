// HTTP/2 server side of a connection (net/http/streams.cpp): the client preface + SETTINGS,
// then frames for each stream the peer opens: HEADERS/CONTINUATION decoded through HPACK and
// the huffman codec (huffman/codec.cpp), DATA with flow control, plus RST/PING/WINDOW_UPDATE/
// GOAWAY handling interleaved by read_frame.
#include <photon/net/http/headers.h>
// H2Connection keeps its Impl behind a unique_ptr with no out-of-line destructor, so a TU
// that destroys one needs the Impl definition; upstream's own test_h2.cpp includes streams.cpp
// for the same reason. The harness does the same (the archive's copy is then not pulled in).
#include "../../net/http/streams.cpp"
#include <memory>
#include "fuzz_common.h"

using namespace photon::net::http;

extern "C" int LLVMFuzzerInitialize(int*, char***) { fuzz_photon_init(); return 0; }

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::unique_ptr<uint8_t[]> copy(new uint8_t[size ? size : 1]);   // ASan-bounded copy
    memcpy(copy.get(), data, size);
    InputStream s(copy.get(), size);
    std::unique_ptr<H2Connection> c(new H2Connection(&s, false));
    auto& conn = *c;
    if (conn.recv_preface() != 0) return 0;
    volatile size_t sink = 0;
    for (int k = 0; k < 16; k++) {
        auto st = conn.accept_stream();
        if (!st) break;
        CommonHeaders<4096> h;
        bool end = false;
        if (st.recv_headers(h, &end) != 0) break;
        for (auto kv : h) sink += kv.first.size() + kv.second.size();
        char buf[4096];
        for (int i = 0; i < 256 && !end; i++) {
            ssize_t n = st.recv_data(buf, sizeof(buf), &end);
            if (n <= 0) break;
            sink += n;
        }
    }
    (void)sink;
    return 0;
}
