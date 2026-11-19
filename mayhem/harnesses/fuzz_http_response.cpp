// HTTP/1 client side: what a client reads back from a server. Response::receive_header over an
// in-memory socket parses the status line + headers (message.cpp, headers.cpp), then the body is
// read to EOF through the body stream the headers select (Content-Length / chunked, body.cpp),
// and the cookie jar ingests the Set-Cookie headers (cookie_jar.cpp) and replays them onto a
// Request.
#include <photon/net/http/message.h>
#include <photon/net/http/client.h>
#include <photon/common/memory-stream/memory-stream.h>
#include <memory>
#include <cstdlib>
#include "fuzz_common.h"

using namespace photon::net::http;

// receive_header is protected (the client's Operation drives it); a subclass is the
// supported way to reach it, as the server/client do.
struct FuzzResponse : public Response {
    using Response::Response;
    using Message::receive_header;
};

extern "C" int LLVMFuzzerInitialize(int*, char***) { fuzz_photon_init(); return 0; }

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    srand(0);   // the in-memory stream chunks reads with rand(): same input, same chunking
    std::unique_ptr<StringSocketStream> s(new_string_socket_stream());
    s->set_input({(const char*)data, size}, true);
    std::unique_ptr<char[]> buf(new char[16 * 1024]);
    FuzzResponse resp(buf.get(), 16 * 1024);
    resp.reset(s.get(), false);
    if (resp.receive_header() != 0) return 0;
    volatile size_t sink = resp.status_code();
    for (auto kv : resp.headers) sink += kv.first.size() + kv.second.size();
    sink += resp.headers["Content-Type"].size();
    sink += resp.resource_size();
    sink += resp.body_size();
    char rb[4096];
    for (int i = 0; i < 4096; i++) {   // bounded: every read consumes input or ends the body
        ssize_t n = resp.read(rb, sizeof(rb));
        if (n <= 0) break;
        sink += n;
    }
    std::unique_ptr<ICookieJar> jar(new_simple_cookie_jar());
    jar->get_cookies_from_headers("www.example.com", &resp);
    char qb[8192];
    Request req(qb, sizeof(qb));
    if (req.reset(Verb::GET, "http://www.example.com/a/b") == 0) {
        jar->set_cookies_to_headers(&req);
        sink += req.headers["Cookie"].size();
    }
    (void)sink;
    return 0;
}
