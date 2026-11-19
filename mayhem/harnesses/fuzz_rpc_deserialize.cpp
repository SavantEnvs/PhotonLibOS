// rpc::DeserializerIOV::deserialize<T> (serialize.h) on a request split across 1-4 iovecs, the
// way it arrives from the network, followed by walking every field: buffers/strings come from
// iovector::extract_front_continuous, the message body from extract_back, and the sorted_map
// values are deserialized lazily on iteration and lookup.
// Byte 0 picks the segment count and split points; the rest is the serialized message.
#include <photon/rpc/serialize.h>
#include <photon/common/iovector.h>
#include <memory>
#include <vector>
#include <cstring>
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
    rpc::sorted_map<rpc::string, MapValue> inner;
    PROCESS_FIELDS(x, s, inner);
};

struct Msg : rpc::Message {
    int64_t i = 0;
    rpc::buffer buf;
    rpc::string str;
    rpc::array<Nested> arr;
    rpc::sorted_map<rpc::string, MapValue> map;
    rpc::iovec_array iovs;
    PROCESS_FIELDS(i, buf, str, arr, map, iovs);
};

template<typename M>
static size_t walk_map(M& m) {
    size_t sink = 0;
    for (auto it = m.begin(); it != m.end(); ++it)
        sink += it->first.size() + it->second.a + it->second.b.size() + it->second.c;
    auto f = m.find("key");
    if (f != m.end()) sink += f->second.a + f->first.size();
    return sink;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return -1;
    uint8_t ctl = data[0];
    data++; size--;
    // Own heap copy so every segment is a separately bounded allocation (ASan sees overreads
    // past a segment end instead of reading into the next one).
    std::vector<std::unique_ptr<char[]>> segs;
    IOVector iov;
    int nseg = (ctl & 3) + 1;
    size_t off = 0;
    for (int k = 0; k < nseg; k++) {
        size_t len = (k == nseg - 1) ? size - off : (size - off) * (((ctl >> (2 + 2 * k)) & 3) + 1) / 5;
        segs.emplace_back(new char[len ? len : 1]);
        memcpy(segs.back().get(), data + off, len);
        if (len) iov.push_back(segs.back().get(), len);
        off += len;
    }
    rpc::DeserializerIOV des;
    Msg* m = des.deserialize<Msg>(&iov);
    if (!m) return 0;
    volatile size_t sink = m->i + m->buf.size() + m->str.size();
    if (m->str.size()) sink += m->str.c_str()[0] + m->str.c_str()[m->str.size() - 1];
    for (auto& n : m->arr) { sink += n.x + n.s.size(); sink += walk_map(n.inner); }
    sink += walk_map(m->map);
    for (auto& v : m->iovs) sink += v.iov_len;
    (void)sink;
    return 0;
}
