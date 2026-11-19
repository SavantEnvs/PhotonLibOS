// SimpleDOM (ecosystem/simple_dom.cpp): the JSON (rapidjson), XML (rapidxml), YAML (rapidyaml)
// and INI document parsers behind photon::SimpleDOM::parse, plus the tree each builds: child
// enumeration, key lookup and value conversion. Byte 0 picks the document type (low 2 bits).
#include <photon/ecosystem/simple_dom.h>
#include "fuzz_common.h"
#include <cstdlib>

using namespace photon::SimpleDOM;

// The root node shares its key/value bit-fields with the document's size/refcount (a union in
// simple_dom_impl.h), so key()/value() are only meaningful on non-root nodes: never read them on
// the root, or every input "crashes" in the harness rather than in the parser.
static size_t walk(const Node& n, int depth) {
    size_t sink = n.type();
    if (!n.is_root()) {
        sink += n.key().size() + n.value().size();
        sink += (size_t)n.to_int64_t();
    }
    if (depth > 64) return sink;
    sink += n.get_attributes().num_children();
    size_t i = 0;
    for (auto c : n.enumerable_children()) {
        sink += walk(c, depth + 1);
        if (i++ == 0) sink += n.get(c.key()).value().size();
    }
    if (n.num_children()) sink += n.get(n.num_children() - 1).key().size();
    return sink;
}

// SimpleDOM is a pure parser (no photon runtime needed); only silence its per-input ERROR logs.
extern "C" int LLVMFuzzerInitialize(int*, char***) {
    set_log_output_level(ALOG_FATAL);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return -1;
    int type = data[0] & 3;   // DOC_JSON, DOC_XML, DOC_YAML, DOC_INI
    // Hand over a full heap copy the way parse(IStream::ReadAll&&) / parse_file do. Not
    // parse_copy(): it strndup()s (stopping at the first NUL) yet passes the full size on, so any
    // input with an embedded NUL over-reads in the harness's own copy and masks every other bug.
    size_t n = size - 1;
    char* text = (char*)malloc(n + 1);
    memcpy(text, data + 1, n);
    text[n] = '\0';
    auto root = parse(text, n, type | DOC_OWN_TEXT);
    if (!root) return 0;
    volatile size_t sink = walk(root, 0);
    sink += root["a"]["b"].value().size();
    (void)sink;
    return 0;
}
