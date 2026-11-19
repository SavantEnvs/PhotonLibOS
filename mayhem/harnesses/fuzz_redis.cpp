// Redis client reply parsing (ecosystem/redis.h/.cpp): RESP simple strings, errors, integers,
// bulk strings and (nested) arrays as a server sends them, read through the client's buffer
// refill logic, as the reply to one command sent with execute().
#include <photon/ecosystem/redis.h>
#include <photon/common/memory-stream/memory-stream.h>
#include <memory>
#include <cstdlib>
#include "fuzz_common.h"

using namespace photon::redis;

extern "C" int LLVMFuzzerInitialize(int*, char***) { fuzz_photon_init(); return 0; }

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return -1;
    srand(0);   // the in-memory stream chunks reads with rand(): same input, same chunking
    std::unique_ptr<StringSocketStream> s(new_string_socket_stream());
    s->set_input({(const char*)data + 1, size - 1}, true);
    RedisClient rc(s.get(), false);
    volatile size_t sink = 0;
    // One command, then its reply, the way a client consumes a server: execute() sends the
    // command and parses the first reply item; an array reply is followed by its elements.
    auto r = rc.execute(data[0] & 1 ? "GET" : "HGETALL", "akey");
    sink += r.mark;
    if (r.is_failed()) sink += r.get_error_message().size();
    else if (r.is_type<bulk_string>()) sink += r.get<bulk_string>().size();
    else if (r.is_type<simple_string>()) sink += r.get<simple_string>().size();
    else if (r.is_type<array_header>()) {
        int64_t n = r.get<array_header>().val;
        for (int64_t i = 0; i < n && i < 64; i++) {
            auto a = rc.parse_response_item();
            sink += a.mark;
            if (a.is_failed()) break;
            if (a.is_type<bulk_string>()) sink += a.get<bulk_string>().size();
        }
    }
    (void)sink;
    return 0;
}
