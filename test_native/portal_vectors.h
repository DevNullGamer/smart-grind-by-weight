#pragma once
// Loader for the shared web panel contract vectors (tools/tests/portal_api_vectors.json), used by
// test_portal_api.cpp and test_portal_guard.cpp. A small strict JSON reader (objects, arrays, strings with the
// usual escapes, numbers, true/false/null): the vectors are data, so a parse failure fails the test run.
#include <unity.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <utility>
#include <vector>

struct PJson {
    enum Kind { NUL, BOOL, NUM, STR, ARR, OBJ } kind = NUL;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<PJson> a;
    std::vector<std::pair<std::string, PJson>> o;

    const PJson* find(const char* key) const {
        for (const auto& kv : o) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    const PJson& at(const char* key) const {
        const PJson* v = find(key);
        if (!v) TEST_FAIL_MESSAGE(key);
        return *v;
    }
    bool is_null() const { return kind == NUL; }
};

namespace portal_vectors_detail {

struct Reader {
    const std::string& t;
    size_t i = 0;
    explicit Reader(const std::string& text) : t(text) {}

    void ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) i++; }
    void expect(char c) {
        ws();
        if (i >= t.size() || t[i] != c) TEST_FAIL_MESSAGE("portal_api_vectors.json: parse error");
        i++;
    }
    std::string str() {
        expect('"');
        std::string out;
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c != '\\') { out += c; continue; }
            const char e = t[i++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    const unsigned code = static_cast<unsigned>(strtoul(t.substr(i, 4).c_str(), nullptr, 16));
                    i += 4;
                    if (code >= 0x80) TEST_FAIL_MESSAGE("portal_api_vectors.json: non-ASCII escape");
                    out += static_cast<char>(code);
                    break;
                }
                default: out += e;  // " \ /
            }
        }
        i++;
        return out;
    }
    PJson value() {
        ws();
        PJson v;
        if (i >= t.size()) TEST_FAIL_MESSAGE("portal_api_vectors.json: unexpected end");
        const char c = t[i];
        if (c == '{') {
            v.kind = PJson::OBJ;
            i++;
            ws();
            if (t[i] == '}') { i++; return v; }
            while (true) {
                std::string k = str();
                expect(':');
                v.o.emplace_back(k, value());
                ws();
                if (t[i] == ',') { i++; continue; }
                expect('}');
                break;
            }
        } else if (c == '[') {
            v.kind = PJson::ARR;
            i++;
            ws();
            if (t[i] == ']') { i++; return v; }
            while (true) {
                v.a.push_back(value());
                ws();
                if (t[i] == ',') { i++; continue; }
                expect(']');
                break;
            }
        } else if (c == '"') {
            v.kind = PJson::STR;
            v.s = str();
        } else if (t.compare(i, 4, "true") == 0) {
            v.kind = PJson::BOOL; v.b = true; i += 4;
        } else if (t.compare(i, 5, "false") == 0) {
            v.kind = PJson::BOOL; v.b = false; i += 5;
        } else if (t.compare(i, 4, "null") == 0) {
            v.kind = PJson::NUL; i += 4;
        } else {
            v.kind = PJson::NUM;
            char* end = nullptr;
            v.n = strtod(t.c_str() + i, &end);
            i = static_cast<size_t>(end - t.c_str());
        }
        return v;
    }
};

}  // namespace portal_vectors_detail

// Parses a JSON text (used for the vectors and for checking the encoders' output)
inline PJson portal_json_parse(const std::string& text) {
    portal_vectors_detail::Reader r(text);
    PJson v = r.value();
    r.ws();
    if (r.i != text.size()) TEST_FAIL_MESSAGE("JSON: trailing bytes");
    return v;
}

// The vectors, loaded once. pio test runs from the project root; the other paths cover a test run started elsewhere.
inline const PJson& portal_vectors() {
    static PJson doc;
    static bool loaded = false;
    if (loaded) return doc;
    static const char* const kPaths[] = {"tools/tests/portal_api_vectors.json", "../tools/tests/portal_api_vectors.json",
                                         "../../tools/tests/portal_api_vectors.json"};
    for (const char* path : kPaths) {
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        std::string text;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
        fclose(f);
        doc = portal_json_parse(text);
        loaded = true;
        return doc;
    }
    TEST_FAIL_MESSAGE("tools/tests/portal_api_vectors.json not found (run pio test from the project root)");
    return doc;
}
