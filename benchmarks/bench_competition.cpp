// Compares simdjson_msgpack with other ways to convert JSON to MessagePack
// and back, in C and C++, on JSON files: best time over several rounds, in
// GB/s of JSON (read, when writing MessagePack; written, when writing JSON).
//   bench_competition [--rounds N] file.json...
//
// JSON to MessagePack:
//   simdjson_msgpack       this library (On-Demand, smallest encodings)
//   simdjson DOM           simdjson's DOM parser, then a walk of the DOM with
//                          the same encodings as ours
//   simdjson benchmark     the On-Demand code of simdjson's json2msgpack
//                          benchmark: every number a float64, every string a
//                          str32, every header 5 bytes (not compact)
//   yyjson                 yyjson's parser, then a walk with our encodings
//   RapidJSON + mpack      RapidJSON's DOM, written with mpack (as msgpack-tools'
//                          json2msgpack does)
//   nlohmann::json         json::parse, then json::to_msgpack
// MessagePack (ours) to JSON:
//   simdjson_msgpack       this library
//   mpack + RapidJSON      mpack's node tree, written with RapidJSON's Writer
//                          (as msgpack-tools' msgpack2json does)
//   mpack + yyjson         mpack's node tree, to a yyjson mutable document,
//                          then yyjson_mut_write
//   nlohmann::json         json::from_msgpack, then dump
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mpack.h"
#include "nlohmann/json.hpp"
#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "simdjson_msgpack.h"
#include "timing.h"
#include "yyjson.h"

namespace {

// MessagePack with known counts and lengths: the encodings of
// simdjson_msgpack, for the DOM walks.
struct compact_writer {
  uint8_t* out;

  void byte(uint8_t b) { *out++ = b; }
  void be(uint64_t v, int n) {
    for (int i = n - 1; i >= 0; i--) *out++ = uint8_t(v >> (8 * i));
  }
  void uint(uint64_t x) {
    if (x < 128) {
      byte(uint8_t(x));
    } else if (x <= 0xff) {
      byte(0xcc), be(x, 1);
    } else if (x <= 0xffff) {
      byte(0xcd), be(x, 2);
    } else if (x <= 0xffffffff) {
      byte(0xce), be(x, 4);
    } else {
      byte(0xcf), be(x, 8);
    }
  }
  void sint(int64_t x) {
    if (x >= 0) return uint(uint64_t(x));
    if (x >= -32) {
      byte(uint8_t(x));
    } else if (x >= -128) {
      byte(0xd0), be(uint64_t(x), 1);
    } else if (x >= -32768) {
      byte(0xd1), be(uint64_t(x), 2);
    } else if (x >= -2147483648LL) {
      byte(0xd2), be(uint64_t(x), 4);
    } else {
      byte(0xd3), be(uint64_t(x), 8);
    }
  }
  void dbl(double d) {
    uint64_t bits;
    std::memcpy(&bits, &d, 8);
    byte(0xcb), be(bits, 8);
  }
  void str(const char* s, size_t n) {
    if (n < 32) {
      byte(uint8_t(0xa0 | n));
    } else if (n <= 0xff) {
      byte(0xd9), be(n, 1);
    } else if (n <= 0xffff) {
      byte(0xda), be(n, 2);
    } else {
      byte(0xdb), be(n, 4);
    }
    std::memcpy(out, s, n);
    out += n;
  }
  void header(size_t n, bool map) {
    if (n < 16) {
      byte(uint8_t((map ? 0x80 : 0x90) | n));
    } else if (n <= 0xffff) {
      byte(map ? 0xde : 0xdc), be(n, 2);
    } else {
      byte(map ? 0xdf : 0xdd), be(n, 4);
    }
  }
};

// simdjson's DOM.
struct simdjson_dom {
  simdjson::dom::parser parser;
  compact_writer w{};

  void value(simdjson::dom::element e) {
    using simdjson::dom::element_type;
    switch (e.type()) {
      case element_type::ARRAY: {
        simdjson::dom::array a = e.get_array().value_unsafe();
        size_t n = a.size();
        if (n == 0xffffff) n = size_t(std::distance(a.begin(), a.end()));  // saturated
        w.header(n, false);
        for (simdjson::dom::element child : a) value(child);
        break;
      }
      case element_type::OBJECT: {
        simdjson::dom::object o = e.get_object().value_unsafe();
        size_t n = o.size();
        if (n == 0xffffff) n = size_t(std::distance(o.begin(), o.end()));
        w.header(n, true);
        for (simdjson::dom::key_value_pair field : o) {
          w.str(field.key.data(), field.key.size());
          value(field.value);
        }
        break;
      }
      case element_type::INT64:
        w.sint(e.get_int64().value_unsafe());
        break;
      case element_type::UINT64:
        w.uint(e.get_uint64().value_unsafe());
        break;
      case element_type::DOUBLE:
        w.dbl(e.get_double().value_unsafe());
        break;
      case element_type::STRING: {
        std::string_view s = e.get_string().value_unsafe();
        w.str(s.data(), s.size());
        break;
      }
      case element_type::BOOL:
        w.byte(e.get_bool().value_unsafe() ? 0xc3 : 0xc2);
        break;
      default:
        w.byte(0xc0);
    }
  }
  bool run(const simdjson::padded_string& json, uint8_t* buffer, size_t* size) {
    simdjson::dom::element root;
    if (parser.parse(json).get(root)) return false;
    w.out = buffer;
    value(root);
    *size = size_t(w.out - buffer);
    return true;
  }
};

// The On-Demand code of simdjson's benchmark/json2msgpack/simdjson_ondemand.h
// (with error codes rather than exceptions): not compact.
struct simdjson_benchmark {
  simdjson::ondemand::parser parser;
  uint8_t* buff{};

  void write_byte(uint8_t b) { *buff++ = b; }
  void write_uint32_at(uint32_t w, uint8_t* p) { std::memcpy(p, &w, 4); }
  void write_double(double d) {
    *buff++ = 0xcb;
    std::memcpy(buff, &d, 8);
    buff += 8;
  }
  bool write_raw_string(simdjson::ondemand::raw_json_string in) {
    write_byte(0xdb);
    uint8_t* location = buff;
    buff += 4;
    std::string_view v;
    if (parser.unescape(in, buff).get(v)) return false;
    write_uint32_at(uint32_t(v.size()), location);
    return true;
  }
  template <typename V>
  bool write_scalar(V& element, simdjson::ondemand::json_type type) {
    switch (type) {
      case simdjson::ondemand::json_type::number: {
        double d;
        if (element.get_double().get(d)) return false;
        write_double(d);
        return true;
      }
      case simdjson::ondemand::json_type::string: {
        simdjson::ondemand::raw_json_string s;
        if (element.get_raw_json_string().get(s)) return false;
        return write_raw_string(s);
      }
      case simdjson::ondemand::json_type::boolean: {
        bool b;
        if (element.get_bool().get(b)) return false;
        write_byte(uint8_t(0xc2 + b));
        return true;
      }
      default: {
        bool n;
        if (element.is_null().get(n) || !n) return false;
        write_byte(0xc0);
        return true;
      }
    }
  }
  bool write_value(simdjson::ondemand::value element) {
    simdjson::ondemand::json_type type;
    if (element.type().get(type)) return false;
    if (type == simdjson::ondemand::json_type::array) {
      uint32_t counter = 0;
      write_byte(0xdd);
      uint8_t* location = buff;
      buff += 4;
      simdjson::ondemand::array a;
      if (element.get_array().get(a)) return false;
      for (auto child : a) {
        simdjson::ondemand::value v;
        if (child.get(v)) return false;
        counter++;
        if (!write_value(v)) return false;
      }
      write_uint32_at(counter, location);
      return true;
    }
    if (type == simdjson::ondemand::json_type::object) {
      uint32_t counter = 0;
      write_byte(0xdf);
      uint8_t* location = buff;
      buff += 4;
      simdjson::ondemand::object o;
      if (element.get_object().get(o)) return false;
      for (auto f : o) {
        simdjson::ondemand::field field;
        if (std::move(f).get(field)) return false;
        counter++;
        if (!write_raw_string(field.key())) return false;
        if (!write_value(field.value())) return false;
      }
      write_uint32_at(counter, location);
      return true;
    }
    return write_scalar(element, type);
  }
  bool run(const simdjson::padded_string& json, uint8_t* buffer, size_t* size) {
    buff = buffer;
    simdjson::ondemand::document doc;
    if (parser.iterate(json).get(doc)) return false;
    simdjson::ondemand::value v;
    if (doc.get_value().get(v)) return false;
    if (!write_value(v)) return false;
    *size = size_t(buff - buffer);
    return true;
  }
};

// yyjson's parser.
struct yyjson_reader {
  compact_writer w{};

  void value(yyjson_val* v) {
    switch (yyjson_get_type(v)) {
      case YYJSON_TYPE_ARR: {
        w.header(yyjson_arr_size(v), false);
        size_t idx, max;
        yyjson_val* child;
        yyjson_arr_foreach(v, idx, max, child) { value(child); }
        break;
      }
      case YYJSON_TYPE_OBJ: {
        w.header(yyjson_obj_size(v), true);
        size_t idx, max;
        yyjson_val *key, *val;
        yyjson_obj_foreach(v, idx, max, key, val) {
          w.str(yyjson_get_str(key), yyjson_get_len(key));
          value(val);
        }
        break;
      }
      case YYJSON_TYPE_STR:
        w.str(yyjson_get_str(v), yyjson_get_len(v));
        break;
      case YYJSON_TYPE_NUM:
        switch (yyjson_get_subtype(v)) {
          case YYJSON_SUBTYPE_UINT:
            w.uint(yyjson_get_uint(v));
            break;
          case YYJSON_SUBTYPE_SINT:
            w.sint(yyjson_get_sint(v));
            break;
          default:
            w.dbl(yyjson_get_real(v));
        }
        break;
      case YYJSON_TYPE_BOOL:
        w.byte(yyjson_get_bool(v) ? 0xc3 : 0xc2);
        break;
      default:
        w.byte(0xc0);
    }
  }
  bool run(const simdjson::padded_string& json, uint8_t* buffer, size_t* size) {
    yyjson_doc* doc = yyjson_read(json.data(), json.size(), 0);
    if (doc == nullptr) return false;
    w.out = buffer;
    value(yyjson_doc_get_root(doc));
    yyjson_doc_free(doc);
    *size = size_t(w.out - buffer);
    return true;
  }
};

// RapidJSON's DOM, written with mpack.
struct rapidjson_mpack {
  static void value(mpack_writer_t* w, const rapidjson::Value& v) {
    if (v.IsObject()) {
      mpack_start_map(w, v.MemberCount());
      for (auto it = v.MemberBegin(); it != v.MemberEnd(); ++it) {
        mpack_write_str(w, it->name.GetString(), it->name.GetStringLength());
        value(w, it->value);
      }
      mpack_finish_map(w);
    } else if (v.IsArray()) {
      mpack_start_array(w, v.Size());
      for (const rapidjson::Value& child : v.GetArray()) value(w, child);
      mpack_finish_array(w);
    } else if (v.IsString()) {
      mpack_write_str(w, v.GetString(), v.GetStringLength());
    } else if (v.IsInt64()) {
      mpack_write_i64(w, v.GetInt64());
    } else if (v.IsUint64()) {
      mpack_write_u64(w, v.GetUint64());
    } else if (v.IsNumber()) {
      mpack_write_double(w, v.GetDouble());
    } else if (v.IsBool()) {
      mpack_write_bool(w, v.GetBool());
    } else {
      mpack_write_nil(w);
    }
  }
  bool run(const simdjson::padded_string& json, uint8_t* buffer, size_t* size, size_t capacity) {
    rapidjson::Document doc;
    doc.Parse<rapidjson::kParseFullPrecisionFlag>(json.data(), json.size());
    if (doc.HasParseError()) return false;
    mpack_writer_t w;
    mpack_writer_init(&w, reinterpret_cast<char*>(buffer), capacity);
    value(&w, doc);
    *size = mpack_writer_buffer_used(&w);
    return mpack_writer_destroy(&w) == mpack_ok;
  }
};

// mpack's node tree, written with RapidJSON's Writer.
struct mpack_rapidjson {
  rapidjson::StringBuffer out;

  template <typename W>
  static bool value(W& w, mpack_node_t node) {
    switch (mpack_node_type(node)) {
      case mpack_type_nil:
        return w.Null();
      case mpack_type_bool:
        return w.Bool(mpack_node_bool(node));
      case mpack_type_int:
        return w.Int64(mpack_node_i64(node));
      case mpack_type_uint:
        return w.Uint64(mpack_node_u64(node));
      case mpack_type_float:
        return w.Double(mpack_node_float(node));
      case mpack_type_double:
        return w.Double(mpack_node_double(node));
      case mpack_type_str:
        return w.String(mpack_node_str(node), mpack_node_strlen(node));
      case mpack_type_array: {
        w.StartArray();
        size_t n = mpack_node_array_length(node);
        for (size_t i = 0; i < n; i++) {
          if (!value(w, mpack_node_array_at(node, i))) return false;
        }
        return w.EndArray();
      }
      case mpack_type_map: {
        w.StartObject();
        size_t n = mpack_node_map_count(node);
        for (size_t i = 0; i < n; i++) {
          mpack_node_t key = mpack_node_map_key_at(node, i);
          if (mpack_node_type(key) != mpack_type_str) return false;
          w.Key(mpack_node_str(key), mpack_node_strlen(key));
          if (!value(w, mpack_node_map_value_at(node, i))) return false;
        }
        return w.EndObject();
      }
      default:
        return false;
    }
  }
  bool run(std::string_view mp, size_t* size) {
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, mp.data(), mp.size());
    mpack_tree_parse(&tree);
    out.Clear();
    rapidjson::Writer<rapidjson::StringBuffer> w(out);
    bool ok = mpack_tree_error(&tree) == mpack_ok && value(w, mpack_tree_root(&tree));
    ok = mpack_tree_destroy(&tree) == mpack_ok && ok;
    *size = out.GetSize();
    return ok;
  }
};

// mpack's node tree, to a yyjson mutable document.
struct mpack_yyjson {
  static yyjson_mut_val* value(yyjson_mut_doc* doc, mpack_node_t node) {
    switch (mpack_node_type(node)) {
      case mpack_type_nil:
        return yyjson_mut_null(doc);
      case mpack_type_bool:
        return yyjson_mut_bool(doc, mpack_node_bool(node));
      case mpack_type_int:
        return yyjson_mut_sint(doc, mpack_node_i64(node));
      case mpack_type_uint:
        return yyjson_mut_uint(doc, mpack_node_u64(node));
      case mpack_type_float:
        return yyjson_mut_real(doc, mpack_node_float(node));
      case mpack_type_double:
        return yyjson_mut_real(doc, mpack_node_double(node));
      case mpack_type_str:
        return yyjson_mut_strn(doc, mpack_node_str(node), mpack_node_strlen(node));
      case mpack_type_array: {
        yyjson_mut_val* a = yyjson_mut_arr(doc);
        size_t n = mpack_node_array_length(node);
        for (size_t i = 0; i < n; i++) {
          yyjson_mut_val* v = value(doc, mpack_node_array_at(node, i));
          if (v == nullptr) return nullptr;
          yyjson_mut_arr_append(a, v);
        }
        return a;
      }
      case mpack_type_map: {
        yyjson_mut_val* o = yyjson_mut_obj(doc);
        size_t n = mpack_node_map_count(node);
        for (size_t i = 0; i < n; i++) {
          mpack_node_t key = mpack_node_map_key_at(node, i);
          if (mpack_node_type(key) != mpack_type_str) return nullptr;
          yyjson_mut_val* k = yyjson_mut_strn(doc, mpack_node_str(key), mpack_node_strlen(key));
          yyjson_mut_val* v = value(doc, mpack_node_map_value_at(node, i));
          if (v == nullptr) return nullptr;
          yyjson_mut_obj_add(o, k, v);
        }
        return o;
      }
      default:
        return nullptr;
    }
  }
  bool run(std::string_view mp, size_t* size) {
    mpack_tree_t tree;
    mpack_tree_init_data(&tree, mp.data(), mp.size());
    mpack_tree_parse(&tree);
    bool ok = mpack_tree_error(&tree) == mpack_ok;
    if (ok) {
      yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
      yyjson_mut_val* root = value(doc, mpack_tree_root(&tree));
      ok = root != nullptr;
      if (ok) {
        yyjson_mut_doc_set_root(doc, root);
        size_t length;
        char* json = yyjson_mut_write(doc, 0, &length);
        ok = json != nullptr;
        *size = length;
        std::free(json);
      }
      yyjson_mut_doc_free(doc);
    }
    return mpack_tree_destroy(&tree) == mpack_ok && ok;
  }
};

struct row {
  const char* name;
  double seconds;
  size_t size;
  int same;  // the same bytes as simdjson_msgpack: 1, different: 0, n/a: -1
};

void print(const std::vector<row>& rows, size_t bytes, const char* what) {
  for (const row& r : rows) {
    if (r.seconds < 0) {
      std::printf("  %-22s %s: failed\n", r.name, what);
      continue;
    }
    std::printf("  %-22s %8.3f GB/s %10zu bytes%s\n", r.name, bytes / r.seconds / 1e9, r.size,
                r.same == 1 ? "" : r.same == 0 ? "  (other bytes)" : "");
  }
}

}  // namespace

int main(int argc, char** argv) {
  int rounds = 5;
  std::vector<const char*> files;
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--rounds") == 0 && i + 1 < argc) {
      rounds = std::atoi(argv[++i]);
    } else {
      files.push_back(argv[i]);
    }
  }
  if (files.empty()) {
    std::fprintf(stderr, "usage: %s [--rounds N] file.json...\n", argv[0]);
    return EXIT_FAILURE;
  }
  for (const char* file : files) {
    simdjson::padded_string json;
    if (simdjson::padded_string::load(file).get(json)) {
      std::fprintf(stderr, "cannot load %s\n", file);
      return EXIT_FAILURE;
    }
    const size_t capacity = simdjson_msgpack::max_msgpack_size(json.size()) + 4 * json.size();
    std::vector<uint8_t> buffer(capacity);
    std::printf("%s (%zu bytes)\n", basename_of(file).c_str(), json.size());

    // JSON to MessagePack.
    std::vector<row> rows;
    simdjson_msgpack::msgpack_writer ours;
    double t = best_time(rounds, [&] {
      ours.clear();
      return ours.write_json(json).ok();
    });
    const std::string reference(ours.view());
    rows.push_back({"simdjson_msgpack", t, reference.size(), -1});
    auto same = [&](size_t size) {
      return size == reference.size() && std::memcmp(buffer.data(), reference.data(), size) == 0;
    };
    {
      simdjson_dom c;
      size_t size = 0;
      t = best_time(rounds, [&] { return c.run(json, buffer.data(), &size); });
      rows.push_back({"simdjson DOM", t, size, same(size)});
    }
    {
      simdjson_benchmark c;
      size_t size = 0;
      t = best_time(rounds, [&] { return c.run(json, buffer.data(), &size); });
      rows.push_back({"simdjson benchmark", t, size, -1});
    }
    {
      yyjson_reader c;
      size_t size = 0;
      t = best_time(rounds, [&] { return c.run(json, buffer.data(), &size); });
      rows.push_back({"yyjson", t, size, same(size)});
    }
    {
      rapidjson_mpack c;
      size_t size = 0;
      t = best_time(rounds, [&] { return c.run(json, buffer.data(), &size, capacity); });
      rows.push_back({"RapidJSON + mpack", t, size, same(size)});
    }
    {
      std::vector<uint8_t> out;
      t = best_time(rounds, [&] {
        out.clear();
        nlohmann::json j = nlohmann::json::parse(json.data(), json.data() + json.size(),
                                                 nullptr, false);
        if (j.is_discarded()) return false;
        nlohmann::json::to_msgpack(j, out);
        return true;
      });
      int s = out.size() == reference.size() &&
              std::memcmp(out.data(), reference.data(), out.size()) == 0;
      rows.push_back({"nlohmann::json", t, out.size(), s});
    }
    print(rows, json.size(), "JSON to MessagePack");

    // MessagePack to JSON.
    rows.clear();
    simdjson_msgpack::json_writer jours;
    t = best_time(rounds, [&] {
      jours.clear();
      return jours.write_msgpack(reference.data(), reference.size()).ok();
    });
    const size_t json_size = jours.view().size();
    std::printf(" MessagePack to JSON (GB/s of the %zu bytes of JSON we write)\n", json_size);
    rows.push_back({"simdjson_msgpack", t, json_size, -1});
    {
      mpack_rapidjson c;
      size_t size = 0;
      t = best_time(rounds, [&] { return c.run(reference, &size); });
      rows.push_back({"mpack + RapidJSON", t, size, -1});
    }
    {
      mpack_yyjson c;
      size_t size = 0;
      t = best_time(rounds, [&] { return c.run(reference, &size); });
      rows.push_back({"mpack + yyjson", t, size, -1});
    }
    {
      std::string out;
      t = best_time(rounds, [&] {
        nlohmann::json j = nlohmann::json::from_msgpack(reference.begin(), reference.end(), true,
                                                        false);
        if (j.is_discarded()) return false;
        out = j.dump();
        return true;
      });
      rows.push_back({"nlohmann::json", t, out.size(), -1});
    }
    print(rows, json_size, "MessagePack to JSON");
  }
  return EXIT_SUCCESS;
}
