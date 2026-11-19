// HTTP/1 server side: a connection's bytes go to HTTPServer::handle_connection (server.cpp),
// which parses each request (message.cpp/headers.cpp), dispatches on the target, and serves
// keep-alive requests until the stream ends. The default handler reads the whole request body
// (Content-Length or chunked, body.cpp); "/ws" is a WebSocket endpoint (websocket.cpp: upgrade
// handshake, then frame header/length/mask parsing, fragmentation and control frames).
#include <photon/net/http/server.h>
#include <photon/net/http/message.h>
#include <photon/net/http/websocket.h>
#include <photon/common/memory-stream/memory-stream.h>
#include <memory>
#include <cstdlib>
#include "fuzz_common.h"

using namespace photon::net::http;

static int body_handler(void*, Request& req, Response& resp, std::string_view) {
    char buf[4096];
    size_t total = 0;
    for (int i = 0; i < 4096; i++) {
        ssize_t n = req.read(buf, sizeof(buf));
        if (n <= 0) break;
        total += n;
    }
    total += req.query().size() + req.host().size();
    resp.set_result(200);
    resp.headers.content_length(1);
    resp.write(total & 1 ? "1" : "0", 1);
    return 0;
}

static int ws_handler(void*, photon::net::http::IWebSocketStream* ws) {
    char buf[4096];
    WebSocketOpcode op;
    for (int i = 0; i < 1024; i++) {
        auto n = ws->recv_frame(buf, sizeof(buf), &op);
        if (n < 0 || op == WebSocketOpcode::Close) break;
        if (op == WebSocketOpcode::Text) ws->send_text({buf, (size_t)n});
    }
    return 0;
}

static HTTPServer* server;

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    fuzz_photon_init();
    // Created once: the server's handler table is configuration, not per-connection state.
    server = new_http_server();
    server->add_handler(new_websocket_handler({nullptr, &ws_handler}), true, "/ws");
    server->add_handler({nullptr, &body_handler});
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    srand(0);   // the in-memory stream chunks reads with rand(): same input, same chunking
    std::unique_ptr<StringSocketStream> s(new_string_socket_stream());
    s->set_input({(const char*)data, size}, true);
    server->handle_connection(s.get());
    return 0;
}
