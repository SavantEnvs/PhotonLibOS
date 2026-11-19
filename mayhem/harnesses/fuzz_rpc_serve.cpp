// RPC server: a connection's bytes go to rpc::Skeleton::serve (rpc.cpp), which reads and checks
// each 40-byte rpc::Header, allocates the request iovector, reads the body and dispatches to the
// registered service; rpc_service then decodes the request with DeserializerIOV (serialize.h)
// before the service touches its fields (buffer, string, array, sorted_map, iovec_array).
// The header magic/version are the wire format's own gate; seeds carry valid headers.
#include <photon/rpc/rpc.h>
#include <photon/rpc/serialize.h>
#include <cstring>
#include <memory>
#include "fuzz_common.h"

namespace rpc = photon::rpc;

struct MapValue : rpc::Message {
    int a = 0;
    rpc::string b;
    char c = 0;
    PROCESS_FIELDS(a, b, c);
};

struct Nested : rpc::Message {
    int x = 0;
    rpc::string s;
    PROCESS_FIELDS(x, s);
};

struct FuzzOp {
    const static uint32_t IID = 0x1;
    const static uint32_t FID = 0x2;
    struct Request : public rpc::Message {
        int32_t code = 0;
        rpc::buffer buf;
        rpc::string str;
        rpc::array<Nested> arr;
        rpc::sorted_map<rpc::string, MapValue> map;
        rpc::iovec_array iovs;
        PROCESS_FIELDS(code, buf, str, arr, map, iovs);
    };
    struct Response : public rpc::Message {
        int32_t sum = 0;
        PROCESS_FIELDS(sum);
    };
};

struct Service {
    int do_rpc_service(FuzzOp::Request* req, FuzzOp::Response* resp, IOVector*, IStream*) {
        volatile size_t sink = req->code + req->buf.size() + req->str.size();
        if (req->str.size()) sink += req->str.c_str()[0];
        for (auto& n : req->arr) sink += n.x + n.s.size();
        for (auto it = req->map.begin(); it != req->map.end(); ++it)
            sink += it->first.size() + it->second.a + it->second.b.size() + it->second.c;
        auto f = req->map.find("key");
        if (f != req->map.end()) sink += f->second.a;
        for (auto& v : req->iovs) sink += v.iov_len;
        resp->sum = (int32_t)sink;
        return 0;
    }
};

static Service service;
static rpc::Skeleton* skeleton;

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    fuzz_photon_init();
    skeleton = rpc::new_skeleton(4);
    skeleton->register_service<FuzzOp>(&service);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Heap copy so reads past the input end are caught by ASan.
    std::unique_ptr<uint8_t[]> copy(new uint8_t[size ? size : 1]);
    memcpy(copy.get(), data, size);
    InputStream s(copy.get(), size);
    skeleton->serve(&s);
    return 0;
}
