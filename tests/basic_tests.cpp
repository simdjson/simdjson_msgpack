// Tests without dependencies: we check the bytes we write.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "simdjson_msgpack.h"

using namespace simdjson_msgpack;

static int failures = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                          \
    }                                                                      \
  } while (0)

static std::string hex(std::string_view s) {
  std::string out;
  char b[4];
  for (unsigned char c : s) {
    std::snprintf(b, sizeof(b), "%02x", c);
    out += b;
  }
  return out;
}

// JSON to MessagePack, which must succeed, in hexadecimal.
static std::string to_msgpack(const std::string& json, msgpack_options options = {}) {
  simdjson::padded_string padded(json);
  std::string out;
  status s = json_to_msgpack(padded, &out, options);
  if (!s.ok()) {
    std::printf("unexpected error for %s: %s\n", json.c_str(), s.message.c_str());
    failures++;
  }
  return hex(out);
}

static std::string to_msgpack_error(const std::string& json) {
  simdjson::padded_string padded(json);
  std::string out;
  status s = json_to_msgpack(padded, &out);
  CHECK(!s.ok());
  CHECK(out.empty());
  return s.message;
}

static std::string unhex(const std::string& h) {
  std::string out;
  for (size_t i = 0; i + 1 < h.size(); i += 2) {
    out += char(std::strtoul(h.substr(i, 2).c_str(), nullptr, 16));
  }
  return out;
}

// MessagePack (in hexadecimal) to JSON, which must succeed.
static std::string to_json(const std::string& h, json_options options = {}) {
  std::string bytes = unhex(h);
  std::string out;
  status s = msgpack_to_json(bytes.data(), bytes.size(), &out, options);
  if (!s.ok()) {
    std::printf("unexpected error for %s: %s\n", h.c_str(), s.message.c_str());
    failures++;
  }
  return out;
}

static std::string to_json_error(const std::string& h, json_options options = {}) {
  std::string bytes = unhex(h);
  std::string out;
  status s = msgpack_to_json(bytes.data(), bytes.size(), &out, options);
  CHECK(!s.ok());
  CHECK(out.empty());
  return s.message;
}

static bool contains(const std::string& haystack, const char* needle) {
  bool found = haystack.find(needle) != std::string::npos;
  if (!found) std::printf("  message: %s\n", haystack.c_str());
  return found;
}

static void test_integers() {
  CHECK(to_msgpack("0") == "00");
  CHECK(to_msgpack("127") == "7f");
  CHECK(to_msgpack("128") == "cc80");
  CHECK(to_msgpack("255") == "ccff");
  CHECK(to_msgpack("256") == "cd0100");
  CHECK(to_msgpack("65535") == "cdffff");
  CHECK(to_msgpack("65536") == "ce00010000");
  CHECK(to_msgpack("4294967295") == "ceffffffff");
  CHECK(to_msgpack("4294967296") == "cf0000000100000000");
  CHECK(to_msgpack("18446744073709551615") == "cfffffffffffffffff");
  CHECK(to_msgpack("-1") == "ff");
  CHECK(to_msgpack("-32") == "e0");
  CHECK(to_msgpack("-33") == "d0df");
  CHECK(to_msgpack("-128") == "d080");
  CHECK(to_msgpack("-129") == "d1ff7f");
  CHECK(to_msgpack("-32768") == "d18000");
  CHECK(to_msgpack("-32769") == "d2ffff7fff");
  CHECK(to_msgpack("-2147483648") == "d280000000");
  CHECK(to_msgpack("-2147483649") == "d3ffffffff7fffffff");
  CHECK(to_msgpack("-9223372036854775808") == "d38000000000000000");
  // Beyond 64 bits: a float64.
  CHECK(to_msgpack("18446744073709551616") == "cb43f0000000000000");
  msgpack_options strict;
  strict.big_integers_as_float64 = false;
  simdjson::padded_string big(std::string("[18446744073709551616]"));
  std::string out;
  CHECK(!json_to_msgpack(big, &out, strict).ok());
}

static void test_floats() {
  CHECK(to_msgpack("1.5") == "cb3ff8000000000000");
  CHECK(to_msgpack("1.0") == "cb3ff0000000000000");
  CHECK(to_msgpack("-0.0") == "cb8000000000000000");
  CHECK(to_msgpack("1e2") == "cb4059000000000000");
  msgpack_options f32;
  f32.float32_when_exact = true;
  CHECK(to_msgpack("1.5", f32) == "ca3fc00000");
  CHECK(to_msgpack("0.1", f32) == "cb3fb999999999999a");
  CHECK(to_json("cb3ff8000000000000") == "1.5");
  CHECK(to_json("cb3ff0000000000000") == "1.0");
  CHECK(to_json("cb8000000000000000") == "-0.0");
  CHECK(to_json("cb4059000000000000") == "100.0");
  CHECK(to_json("cb3fb999999999999a") == "0.1");
  CHECK(to_json("cb7ff8000000000000") == "null");  // NaN
  CHECK(to_json("cb7ff0000000000000") == "null");  // infinity
  CHECK(to_json("ca3fc00000") == "1.5");
  CHECK(to_json("ca3dcccccd") == "0.1");  // float32 0.1, shortest
  CHECK(to_json("ca40000000") == "2.0");
  CHECK(to_json("cb4415af1d78b58c40") == "1e+20");
}

static void test_scalars() {
  CHECK(to_msgpack("true") == "c3");
  CHECK(to_msgpack("false") == "c2");
  CHECK(to_msgpack("null") == "c0");
  CHECK(to_msgpack("  null  ") == "c0");
  CHECK(to_msgpack("\"\"") == "a0");
  CHECK(to_msgpack("\"abc\"") == "a3616263");
  CHECK(to_msgpack("\"a\\nb\\u00e9\"") == "a5610a62c3a9");
  CHECK(to_json("c3") == "true");
  CHECK(to_json("c2") == "false");
  CHECK(to_json("c0") == "null");
  CHECK(to_json("00") == "0");
  CHECK(to_json("ff") == "-1");
  CHECK(to_json("cfffffffffffffffff") == "18446744073709551615");
  CHECK(to_json("d38000000000000000") == "-9223372036854775808");
  CHECK(to_json("d1ff7f") == "-129");
}

static void test_strings() {
  // Header sizes: fixstr below 32 bytes, str8 below 256, str16, str32.
  for (size_t n : {0, 1, 31, 32, 33, 255, 256, 257, 65535, 65536, 70000}) {
    std::string s(n, 'x');
    for (size_t i = 0; i < n; i += 7) s[i] = char('a' + i % 26);
    std::string mp = unhex(to_msgpack("\"" + s + "\""));
    size_t header = n < 32 ? 1 : n < 256 ? 2 : n < 65536 ? 3 : 5;
    CHECK(mp.size() == header + n);
    CHECK(mp.substr(header) == s);
    if (n < 32) CHECK(uint8_t(mp[0]) == (0xa0 | n));
    if (n >= 32 && n < 256) CHECK(uint8_t(mp[0]) == 0xd9);
    if (n >= 256 && n < 65536) CHECK(uint8_t(mp[0]) == 0xda);
    if (n >= 65536) CHECK(uint8_t(mp[0]) == 0xdb);
    std::string json;
    CHECK(msgpack_to_json(mp.data(), mp.size(), &json).ok());
    CHECK(json == "\"" + s + "\"");
  }
  // Escaping.
  CHECK(to_json("a6223c5c0a011f") == "\"\\\"<\\\\\\n\\u0001\\u001f\"");
  CHECK(to_json("a2c3a9") == "\"\xc3\xa9\"");
  CHECK(contains(to_json_error("a2c3c3"), "invalid UTF-8"));
  json_options lax;
  lax.validate_utf8 = false;
  CHECK(to_json("a2c3c3", lax) == "\"\xc3\xc3\"");
  // A long string with an escape after the first 8 bytes.
  CHECK(to_json("aa61626364656667686922") == "\"abcdefghi\\\"\"");
}

static void test_containers() {
  CHECK(to_msgpack("[]") == "90");
  CHECK(to_msgpack("{}") == "80");
  CHECK(to_msgpack("[1,[2,[]],{\"a\":null}]") == "930192029081a161c0");
  CHECK(to_msgpack("{\"a\":1,\"a\":2}") == "82a16101a16102");  // duplicate keys kept
  // Header sizes: fixarray below 16, array16 below 65536, array32.
  for (size_t n : {15, 16, 65535, 65536}) {
    std::string json = "[";
    std::string jobj = "{";
    for (size_t i = 0; i < n; i++) {
      if (i) json += ",", jobj += ",";
      json += "1";
      jobj += "\"\":1";
    }
    json += "]";
    jobj += "}";
    std::string a = unhex(to_msgpack(json));
    std::string o = unhex(to_msgpack(jobj));
    size_t header = n < 16 ? 1 : n < 65536 ? 3 : 5;
    CHECK(a.size() == header + n);
    CHECK(o.size() == header + 2 * n);
    if (n == 16) CHECK(hex(a.substr(0, 3)) == "dc0010");
    if (n == 65536) CHECK(hex(a.substr(0, 5)) == "dd00010000");
    if (n == 65536) CHECK(hex(o.substr(0, 5)) == "df00010000");
    std::string back;
    CHECK(msgpack_to_json(a.data(), a.size(), &back).ok());
    CHECK(back == json);
    back.clear();
    CHECK(msgpack_to_json(o.data(), o.size(), &back).ok());
    CHECK(back == jobj);
  }
  // Nested containers with long strings in between (the headers move).
  std::string long_string(300, 'z');
  std::string json = "{\"k\":[\"" + long_string + "\",{\"x\":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17]}],\"" +
                     long_string + "\":-1}";
  std::string mp = unhex(to_msgpack(json));
  std::string back;
  CHECK(msgpack_to_json(mp.data(), mp.size(), &back).ok());
  CHECK(back == json);
  CHECK(to_json("92c0c3") == "[null,true]");
  CHECK(to_json("82a16101a16292c2c0") == "{\"a\":1,\"b\":[false,null]}");
}

static void test_map_keys() {
  CHECK(to_json("8101c0") == "{\"1\":null}");
  CHECK(to_json("81ffc0") == "{\"-1\":null}");
  CHECK(to_json("81c3c0") == "{\"true\":null}");
  CHECK(to_json("81c0c0") == "{\"null\":null}");
  CHECK(to_json("81cb3ff8000000000000c0") == "{\"1.5\":null}");
  CHECK(to_json("81c40161c0") == "{\"YQ==\":null}");
  CHECK(contains(to_json_error("819001"), "map key"));
  CHECK(contains(to_json_error("818001"), "map key"));
}

static void test_binary_ext() {
  CHECK(to_json("c400") == "\"\"");
  CHECK(to_json("c40161") == "\"YQ==\"");
  CHECK(to_json("c4026162") == "\"YWI=\"");
  CHECK(to_json("c403616263") == "\"YWJj\"");
  CHECK(to_json("c50004616263ff") == "\"YWJj/w==\"");
  json_options no_binary;
  no_binary.binary_as_base64 = false;
  CHECK(contains(to_json_error("c40161", no_binary), "binary"));
  CHECK(contains(to_json_error("d40501"), "extension"));
  json_options ext_null;
  ext_null.ext = ext_policy::null;
  CHECK(to_json("92d40501c0", ext_null) == "[null,null]");
  CHECK(to_json("91d6ff00000001", ext_null) == "[null]");  // a timestamp
  CHECK(contains(to_json_error("92d6ff00000001", ext_null), "truncated"));
  CHECK(to_json("92c70305616263c3", ext_null) == "[null,true]");
}

static void test_msgpack_errors() {
  CHECK(contains(to_json_error(""), "truncated input (byte 0)"));
  CHECK(contains(to_json_error("c1"), "0xc1"));
  CHECK(contains(to_json_error("92c0"), "truncated input (byte 2)"));
  CHECK(contains(to_json_error("a3616"), "truncated"));
  CHECK(contains(to_json_error("cd01"), "truncated"));
  CHECK(contains(to_json_error("dbffffffff"), "truncated string"));
  CHECK(contains(to_json_error("ddffffffff"), "truncated"));
  CHECK(contains(to_json_error("c0c0"), "unexpected bytes after the value (byte 1)"));
  std::string deep;
  for (int i = 0; i < 2000; i++) deep += "91";
  deep += "c0";
  CHECK(contains(to_json_error(deep), "nested too deeply"));
  json_options deeper;
  deeper.max_depth = 4000;
  CHECK(to_json(deep, deeper).size() == 2000 * 2 + 4);
}

static void test_json_errors() {
  CHECK(!to_msgpack_error("[1,2").empty());
  CHECK(!to_msgpack_error("[1,2]]").empty());
  CHECK(!to_msgpack_error("{\"a\":}").empty());
  CHECK(!to_msgpack_error("nul").empty());
  CHECK(!to_msgpack_error("[nul]").empty());
  CHECK(!to_msgpack_error("\"\\x\"").empty());
  CHECK(!to_msgpack_error("1 2").empty());
  CHECK(!to_msgpack_error("").empty());
  CHECK(!to_msgpack_error("[1e400]").empty());
  // A failed write leaves the writer as it was.
  msgpack_writer w;
  simdjson::padded_string good(std::string("[1]"));
  simdjson::padded_string bad(std::string("[1,"));
  CHECK(w.write_json(good).ok());
  CHECK(!w.write_json(bad).ok());
  CHECK(hex(w.view()) == "9101");
}

static void test_ndjson() {
  std::string ndjson = "{\"a\":1}\n[2]\n\"x\"\n 3 \n{\"b\":[true,null]}\n";
  simdjson::padded_string padded(ndjson);
  std::string mp;
  CHECK(ndjson_to_msgpack(padded, &mp).ok());
  CHECK(hex(mp) == "81a16101" "9102" "a178" "03" "81a16292c3c0");
  std::string back;
  CHECK(msgpack_to_ndjson(mp.data(), mp.size(), &back).ok());
  CHECK(back == "{\"a\":1}\n[2]\n\"x\"\n3\n{\"b\":[true,null]}\n");
  size_t count = 0;
  json_writer jw;
  CHECK(jw.write_msgpack_stream(mp.data(), mp.size(), &count).ok());
  CHECK(count == 5);
  // Many documents, on several threads, with small blocks: same output.
  std::string big;
  for (int i = 0; i < 20000; i++) {
    big += "{\"id\":" + std::to_string(i * 7919) + ",\"name\":\"user" + std::to_string(i) +
           "\",\"tags\":[\"a\",\"b\"],\"score\":" + std::to_string(i) + ".5}\n";
  }
  simdjson::padded_string pbig(big);
  std::string one, many;
  CHECK(ndjson_to_msgpack(pbig, &one).ok());
  msgpack_options threaded;
  threaded.threads = 8;
  threaded.block_size = 4096;
  CHECK(ndjson_to_msgpack(pbig, &many, threaded).ok());
  CHECK(one == many);
  msgpack_writer counter(threaded);
  count = 0;
  CHECK(counter.write_ndjson(pbig, &count).ok());
  CHECK(count == 20000);
  // A document spanning lines, with threads: we convert again in one thread.
  std::string spanning = big + "{\"a\":\n1}\n" + big;
  simdjson::padded_string pspanning(spanning);
  std::string a, b;
  CHECK(ndjson_to_msgpack(pspanning, &a).ok());
  CHECK(ndjson_to_msgpack(pspanning, &b, threaded).ok());
  CHECK(a == b);
  // Errors are the same with threads.
  std::string broken = big + "{\"a\":}\n" + big;
  simdjson::padded_string pbroken(broken);
  std::string e1, e2;
  status s1 = ndjson_to_msgpack(pbroken, &e1);
  status s2 = ndjson_to_msgpack(pbroken, &e2, threaded);
  CHECK(!s1.ok());
  CHECK(s1.message == s2.message);
  CHECK(contains(s1.message, "document 20000"));
  // MessagePack to NDJSON on several threads: the same output, the same errors.
  std::string back_one, back_many;
  CHECK(msgpack_to_ndjson(one.data(), one.size(), &back_one).ok());
  json_options jthreaded;
  jthreaded.threads = 8;
  jthreaded.block_size = 4096;
  CHECK(msgpack_to_ndjson(one.data(), one.size(), &back_many, jthreaded).ok());
  CHECK(back_one == back_many);
  CHECK(back_one.size() > 20000);
  json_writer jcounter(jthreaded);
  count = 0;
  CHECK(jcounter.write_msgpack_stream(one.data(), one.size(), &count).ok());
  CHECK(count == 20000);
  for (const std::string& damaged : {one.substr(0, one.size() - 1), one + "\xc1",
                                      one.substr(0, one.size() / 2) + "\xc1"}) {
    std::string o1, o2;
    status d1 = msgpack_to_ndjson(damaged.data(), damaged.size(), &o1);
    status d2 = msgpack_to_ndjson(damaged.data(), damaged.size(), &o2, jthreaded);
    CHECK(!d1.ok());
    CHECK(d1.message == d2.message);
    CHECK(o2.empty());
  }
  // A document larger than the block size.
  std::string huge = "[" + std::string(3 << 20, ' ') + "1]\n[2]\n";
  simdjson::padded_string phuge(huge);
  std::string h;
  CHECK(ndjson_to_msgpack(phuge, &h).ok());
  CHECK(hex(h) == "91019102");
}

static void test_round_trip() {
  const char* docs[] = {
      R"({"id":12345,"name":"caf\u00e9 \"quoted\"","list":[1,-2,3.25,1e300,-1e-300,true,false,null],"nested":{"a":{"b":{"c":[[],{}]}}}})",
      R"([0,-0.0,0.5,123456789012,-123456789012,9223372036854775807,18446744073709551615])",
      R"({"emoji":"😀","tab":"a\tb","ctl":"\u0001"})",
  };
  for (const char* d : docs) {
    std::string mp = unhex(to_msgpack(d));
    std::string json;
    CHECK(msgpack_to_json(mp.data(), mp.size(), &json).ok());
    // Writing back the JSON gives the same MessagePack.
    CHECK(hex(mp) == to_msgpack(json));
  }
}

static void test_allocator() {
  struct counter {
    size_t calls{0};
  } c;
  allocator a{[](void* ctx, char* p, size_t, size_t size) -> char* {
                static_cast<counter*>(ctx)->calls++;
                return static_cast<char*>(std::realloc(p, size));
              },
              [](void*, char* p) { std::free(p); }, &c};
  msgpack_writer w({}, a);
  simdjson::padded_string json(std::string("[1,2,3]"));
  CHECK(w.write_json(json).ok());
  size_t size;
  char* p = w.release(&size);
  CHECK(size == 4);
  CHECK(hex(std::string_view(p, size)) == "93010203");
  std::free(p);
  CHECK(c.calls >= 1);
  json_writer jw({}, a);
  CHECK(jw.write_msgpack("\x93\x01\x02\x03", 4).ok());
  p = jw.release(&size);
  CHECK(std::string(p) == "[1,2,3]");
  std::free(p);
}

static void test_c_api() {
  char* out = nullptr;
  size_t length = 0;
  char error[256];
  const char json[] = "{\"a\":[1,2]}";
  CHECK(simdjson_msgpack_json_to_msgpack(json, sizeof(json) - 1, sizeof(json), 0, 0, &out,
                                         &length, error, sizeof(error)) == 0);
  CHECK(hex(std::string_view(out, length)) == "81a161920102");
  char* back = nullptr;
  size_t back_length = 0;
  CHECK(simdjson_msgpack_msgpack_to_json(out, length, 0, 1, 1, 0, 0, &back, &back_length, error,
                                         sizeof(error)) == 0);
  CHECK(std::string(back, back_length) == json);
  simdjson_msgpack_free(out);
  simdjson_msgpack_free(back);
  CHECK(simdjson_msgpack_msgpack_to_json("\xc1", 1, 0, 1, 1, 0, 0, &back, &back_length, error,
                                         sizeof(error)) != 0);
  CHECK(contains(error, "0xc1"));
  CHECK(simdjson_msgpack_json_to_msgpack("[1,", 3, 3, 0, 0, &out, &length, error,
                                         sizeof(error)) != 0);
}


// Random JSON documents, built from a seed.
struct random_json {
  uint64_t state;
  uint64_t next() {
    state += 0x9e3779b97f4a7c15;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
  }
  size_t below(size_t n) { return size_t(next() % n); }
  void string(std::string& out) {
    static const char* pieces[] = {"a", "b", "z", "0", " ", "\\\"", "\\\\", "\\/", "\\n",
                                   "\\t", "\\u0001", "\\u00e9", "\\ud83d\\ude00", "\xc3\xa9",
                                   "\xe2\x82\xac", "\xf0\x9f\x98\x80", "\x7f"};
    size_t n = below(10) == 0 ? below(300) : below(12);
    out += '"';
    for (size_t i = 0; i < n; i++) out += pieces[below(sizeof(pieces) / sizeof(pieces[0]))];
    out += '"';
  }
  void number(std::string& out) {
    switch (below(6)) {
      case 0:
        out += std::to_string(int64_t(below(300)) - 40);
        break;
      case 1:
        out += std::to_string(int64_t(next()));
        break;
      case 2:
        out += std::to_string(next());
        break;
      case 3:
        out += std::to_string(int64_t(next()) >> below(64));
        break;
      default: {
        char b[40];
        std::snprintf(b, sizeof(b), "%.17g", (double(int64_t(next())) / 1e9) * (below(2) ? 1e-5 : 1e5));
        out += b;
      }
    }
  }
  void value(std::string& out, int depth) {
    size_t r = below(20);
    if (depth < 6 && r < 3) {
      size_t n = below(8) == 0 ? below(40) : below(5);
      out += '[';
      for (size_t i = 0; i < n; i++) {
        if (i) out += ',';
        value(out, depth + 1);
      }
      out += ']';
    } else if (depth < 6 && r < 6) {
      size_t n = below(8) == 0 ? below(40) : below(5);
      out += '{';
      for (size_t i = 0; i < n; i++) {
        if (i) out += ',';
        string(out);
        out += below(2) ? ":" : " : ";
        value(out, depth + 1);
      }
      out += '}';
    } else if (r < 10) {
      string(out);
    } else if (r < 17) {
      number(out);
    } else {
      out += r == 17 ? "true" : r == 18 ? "false" : "null";
    }
  }
};

// JSON to MessagePack to JSON to MessagePack: the same MessagePack, and the
// JSON is valid for simdjson.
static void test_random_round_trips() {
  random_json gen{1};
  simdjson::dom::parser dom;
  for (int i = 0; i < 20000; i++) {
    std::string json;
    gen.value(json, 0);
    simdjson::padded_string padded(json);
    std::string mp, back, mp2;
    status s = json_to_msgpack(padded, &mp);
    if (!s.ok()) {
      std::printf("unexpected error %s for %s\n", s.message.c_str(), json.c_str());
      failures++;
      continue;
    }
    CHECK(msgpack_to_json(mp.data(), mp.size(), &back).ok());
    simdjson::dom::element e;
    CHECK(dom.parse(back).get(e) == simdjson::SUCCESS);
    simdjson::padded_string padded_back(back);
    CHECK(json_to_msgpack(padded_back, &mp2).ok());
    if (mp != mp2) {
      std::printf("round trip differs: %s -> %s\n", json.c_str(), back.c_str());
      failures++;
    }
  }
}

// Random changes to valid MessagePack: an error, or JSON that simdjson accepts.
static void test_msgpack_mutations() {
  random_json gen{2};
  simdjson::dom::parser dom;
  json_options options;
  options.ext = ext_policy::null;
  for (int i = 0; i < 50000; i++) {
    std::string json;
    gen.value(json, 0);
    simdjson::padded_string padded(json);
    std::string mp;
    if (!json_to_msgpack(padded, &mp).ok()) continue;
    for (size_t k = 1 + gen.below(4); k > 0 && !mp.empty(); k--) {
      size_t at = gen.below(mp.size());
      switch (gen.below(3)) {
        case 0:
          mp[at] = char(gen.below(256));
          break;
        case 1:
          mp.resize(at);
          break;
        default:
          mp.insert(mp.begin() + at, char(gen.below(256)));
      }
    }
    std::string out;
    if (!msgpack_to_json(mp.data(), mp.size(), &out, options).ok()) {
      CHECK(out.empty());
      continue;
    }
    simdjson::dom::element e;
    if (dom.parse(out).get(e) != simdjson::SUCCESS) {
      std::printf("invalid JSON written: %s\n", out.c_str());
      failures++;
    }
  }
}

// Random changes to valid JSON: we accept it if and only if simdjson's DOM
// parser does (except for integers beyond 64 bits, which the DOM parser
// rejects).
static void test_json_mutations() {
  random_json gen{3};
  simdjson::dom::parser dom;
  static const char chars[] = "[]{},:\"0123456789-.eE+tfn \\u";
  for (int i = 0; i < 50000; i++) {
    std::string json;
    gen.value(json, 0);
    for (size_t k = 1 + gen.below(3); k > 0 && !json.empty(); k--) {
      size_t at = gen.below(json.size());
      if (gen.below(2)) {
        json[at] = chars[gen.below(sizeof(chars) - 1)];
      } else {
        json.erase(at, 1);
      }
    }
    simdjson::padded_string padded(json);
    std::string mp;
    const bool ours = json_to_msgpack(padded, &mp).ok();
    simdjson::dom::element e;
    const simdjson::error_code theirs = dom.parse(padded).get(e);
    if (theirs == simdjson::BIGINT_ERROR) continue;
    if (ours != (theirs == simdjson::SUCCESS)) {
      std::printf("we %s %s\n", ours ? "accept" : "reject", json.c_str());
      failures++;
    }
  }
}

int main() {
  test_integers();
  test_floats();
  test_scalars();
  test_strings();
  test_containers();
  test_map_keys();
  test_binary_ext();
  test_msgpack_errors();
  test_json_errors();
  test_ndjson();
  test_round_trip();
  test_allocator();
  test_c_api();
  test_random_round_trips();
  test_msgpack_mutations();
  test_json_mutations();
  if (failures != 0) {
    std::printf("%d failures\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("all tests passed\n");
  return EXIT_SUCCESS;
}
