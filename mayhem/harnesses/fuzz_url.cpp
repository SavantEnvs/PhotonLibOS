// URL parsing (net/http/url.cpp): URL::from_string on a non-NUL-terminated heap copy, the
// accessors, url_escape/url_unescape, and Request::reset(verb, url), which parses the URL a
// client is asked to fetch and writes it into the request line + Host header.
#include <photon/net/http/url.h>
#include <photon/net/http/message.h>
#include <photon/net/http/verb.h>
#include <memory>
#include <string>
#include <cstring>
#include "fuzz_common.h"

using namespace photon::net::http;

extern "C" int LLVMFuzzerInitialize(int*, char***) { fuzz_photon_init(); return 0; }

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // URL keeps a `const char*` to the caller's text and its from_string() DEFER rebuilds a
    // view from it with strlen (url.cpp:49), so URL needs NUL-terminated text: StoredURL, the
    // owning variant, strndup()s for exactly that reason (url.h). URL/Request get a
    // NUL-terminated heap copy; StoredURL gets the raw, unterminated bytes.
    std::unique_ptr<char[]> raw(new char[size ? size : 1]);
    memcpy(raw.get(), data, size);
    std::unique_ptr<char[]> buf(new char[size + 1]);
    memcpy(buf.get(), data, size);
    buf[size] = '\0';
    std::string_view sv(buf.get(), size);
    std::string_view rawsv(raw.get(), size);
    volatile size_t sink = 0;
    {
        URL u(sv);
        sink += u.host().size() + u.host_port().size() + u.path().size() +
                u.query().size() + u.target().size() + u.port() + u.secure();
        sink += need_optional_port(u);
    }
    {
        StoredURL su(rawsv);
        sink += su.target().size() + su.host().size();
    }
    std::string esc = url_escape(rawsv);
    std::string unesc = url_unescape(rawsv);
    std::string round = url_unescape(esc);
    sink += unesc.size() + round.size();
    {
        char rbuf[4096];
        Request req(rbuf, sizeof(rbuf));
        if (req.reset(Verb::GET, sv) == 0)
            sink += req.target().size() + req.headers["Host"].size();
    }
    (void)sink;
    return 0;
}
