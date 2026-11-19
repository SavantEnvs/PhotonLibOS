// Address parsing (net/socket.h, net/utils.cpp, basic_socket.cpp): IPAddr from text,
// EndPoint::parse ("host:port", "[v6]:port"), parse_address_list (a comma/space separated
// endpoint list as found in configuration), and the estring numeric helpers they use.
#include <photon/net/socket.h>
#include <photon/net/utils.h>
#include <photon/common/estring.h>
#include <string>
#include <vector>
#include "fuzz_common.h"

using namespace photon::net;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string str((const char*)data, size);   // NUL-terminated copy for the const char* API
    std::string_view sv(str);
    volatile size_t sink = 0;
    IPAddr ip(str.c_str());
    sink += ip.undefined() + ip.is_ipv4();
    auto ep = EndPoint::parse(sv, 80);
    sink += ep.port + ep.undefined();
    std::vector<EndPoint> list;
    if (parse_address_list(sv, &list, 80) == 0) sink += list.size();
    estring_view es(sv);
    uint64_t u = 0; int64_t i = 0;
    if (!es.empty()) sink += es.to_uint64_check(&u) + es.to_int64_check(&i) + u + i;
    for (auto part : es.split(",: ")) sink += part.size();
    (void)sink;
    return 0;
}
