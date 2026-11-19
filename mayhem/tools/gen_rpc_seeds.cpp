// Seed generator for the rpc_serve / rpc_deserialize targets (not part of the build; run once,
// its output is committed under mayhem/<target>/testsuite/). Serializes the harnesses' message
// types with photon's own SerializerIOV so the seeds are valid wire messages.
#include <photon/rpc/rpc.h>
#include <photon/rpc/serialize.h>
#include <cstdio>
#include <string>
#include <vector>
namespace rpc = photon::rpc;
struct MapValue : rpc::Message { int a = 0; rpc::string b; char c = 0; PROCESS_FIELDS(a, b, c); };
struct NestedS : rpc::Message { int x = 0; rpc::string s; PROCESS_FIELDS(x, s); };
struct NestedD : rpc::Message { int x = 0; rpc::string s; rpc::sorted_map<rpc::string, MapValue> inner; PROCESS_FIELDS(x, s, inner); };
struct ReqS : rpc::Message {
    int32_t code = 0; rpc::buffer buf; rpc::string str; rpc::array<NestedS> arr;
    rpc::sorted_map<rpc::string, MapValue> map; rpc::iovec_array iovs;
    PROCESS_FIELDS(code, buf, str, arr, map, iovs);
};
struct MsgD : rpc::Message {
    int64_t i = 0; rpc::buffer buf; rpc::string str; rpc::array<NestedD> arr;
    rpc::sorted_map<rpc::string, MapValue> map; rpc::iovec_array iovs;
    PROCESS_FIELDS(i, buf, str, arr, map, iovs);
};
template<typename T> static std::string flat(T& m) {
    rpc::SerializerIOV s; s.serialize(m);
    std::string out;
    for (auto& v : s.iov) out.append((char*)v.iov_base, v.iov_len);
    return out;
}
static void put(const char* path, const std::string& d) {
    FILE* f = fopen(path, "wb"); fwrite(d.data(), 1, d.size(), f); fclose(f);
}
template<typename M> static void fill(M& m, rpc::sorted_map_factory<rpc::string, MapValue>& fac,
                                      std::vector<MapValue>& vals, char* payload, iovec* iv) {
    m.buf.assign(payload, 8);
    m.str = "hello";
    vals.resize(3);
    for (int k = 0; k < 3; k++) { vals[k].a = k + 1; vals[k].b = "v"; vals[k].c = 'a' + k; }
    fac.append("key", vals[0]); fac.append("a", vals[1]); fac.append("zz", vals[2]);
    fac.assign_to(&m.map);
    iv[0] = {payload, 4}; iv[1] = {payload + 4, 4};
    m.iovs.assign(iv, 2);
}
int main(int argc, char** argv) {
    std::string dir = argv[1];
    char payload[8] = {'p','a','y','l','o','a','d','!'};
    iovec iv[2];
    {   // rpc_serve: Header + ReqS, twice (keep-alive), FuzzOp IID=1 FID=2
        ReqS r; rpc::sorted_map_factory<rpc::string, MapValue> fac; std::vector<MapValue> vals;
        fill(r, fac, vals, payload, iv);
        r.code = 7;
        NestedS ns[2]; ns[0].x = 1; ns[0].s = "n0"; ns[1].x = 2; ns[1].s = "n1";
        r.arr.assign(ns, 2);
        std::string body = flat(r);
        std::string out;
        for (int t = 0; t < 2; t++) {
            rpc::Header h; h.size = body.size(); h.function = rpc::FunctionID(1, 2); h.tag = t + 1;
            out.append((char*)&h, sizeof(h)); out += body;
        }
        put((dir + "/rpc_serve/testsuite/two_requests.bin").c_str(), out);
        ReqS e; std::string eb = flat(e);
        rpc::Header h; h.size = eb.size(); h.function = rpc::FunctionID(1, 2); h.tag = 1;
        put((dir + "/rpc_serve/testsuite/empty_request.bin").c_str(), std::string((char*)&h, sizeof(h)) + eb);
    }
    {   // rpc_deserialize: control byte + MsgD
        MsgD m; rpc::sorted_map_factory<rpc::string, MapValue> fac; std::vector<MapValue> vals;
        fill(m, fac, vals, payload, iv);
        m.i = -5;
        NestedD nd[1]; nd[0].x = 3; nd[0].s = "nested";
        m.arr.assign(nd, 1);
        std::string body = flat(m);
        put((dir + "/rpc_deserialize/testsuite/one_segment.bin").c_str(), std::string(1, '\x00') + body);
        put((dir + "/rpc_deserialize/testsuite/four_segments.bin").c_str(), std::string(1, '\xe7') + body);
        MsgD e; put((dir + "/rpc_deserialize/testsuite/empty.bin").c_str(), std::string(1, '\x01') + flat(e));
    }
    return 0;
}
