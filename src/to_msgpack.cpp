// Converts JSON to MessagePack, with simdjson's On-Demand parser.
//
// On-Demand gives the elements of an array or an object one after the other:
// we do not know how many there are when we write the header. So we leave 5
// bytes (the largest header) for each array and object header, note where,
// and once the document is written, move the bytes back to put the smallest
// header in place (one pass over the output). A string is unescaped right
// after one byte left for its header (strings are mostly short), and moved
// forward if it needs a longer header.
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#include "simdjson_msgpack.h"

#include "buffer.h"
#include "parallel.h"

namespace simdjson_msgpack {
namespace {

using namespace simdjson;

simdjson_inline uint8_t* put16(uint8_t* p, uint16_t v) {
  p[0] = uint8_t(v >> 8);
  p[1] = uint8_t(v);
  return p + 2;
}
simdjson_inline uint8_t* put32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
  return p + 4;
}
simdjson_inline uint8_t* put64(uint8_t* p, uint64_t v) {
  p = put32(p, uint32_t(v >> 32));
  return put32(p, uint32_t(v));
}

// Where the header of an array or an object goes: 5 bytes at offset (from
// the start of the document).
struct header {
  size_t offset;
  uint32_t count;
  bool is_map;
};
constexpr size_t header_room = 5;

simdjson_inline uint8_t* put_header(uint8_t* p, const header& h) {
  if (h.count < 16) {
    *p = uint8_t((h.is_map ? 0x80 : 0x90) | h.count);
    return p + 1;
  }
  if (h.count <= 0xffff) {
    *p = h.is_map ? 0xde : 0xdc;
    return put16(p + 1, uint16_t(h.count));
  }
  *p = h.is_map ? 0xdf : 0xdd;
  return put32(p + 1, h.count);
}

// Puts the smallest headers in place in the document [begin, end), moving
// back what follows them; returns the new end.
uint8_t* compact(uint8_t* begin, uint8_t* end, const std::vector<header>& headers) {
  if (headers.empty()) return end;
  uint8_t* out = begin + headers[0].offset;
  const uint8_t* in = out;
  for (const header& h : headers) {
    const uint8_t* at = begin + h.offset;
    const size_t n = size_t(at - in);
    std::memmove(out, in, n);
    out = put_header(out + n, h);
    in = at + header_room;
  }
  const size_t n = size_t(end - in);
  std::memmove(out, in, n);
  return out + n;
}

// Writes one JSON document as MessagePack. The output must have room for
// max_msgpack_size(the document's length) bytes.
class encoder {
 public:
  encoder(ondemand::parser& parser, std::vector<header>& headers, const msgpack_options& options)
      : parser_(parser), headers_(headers), options_(options) {}

  // Writes the document at out; sets *end past the value.
  template <typename document>
  error_code write(document& doc, uint8_t* out, uint8_t** end) {
    begin_ = out_ = out;
    headers_.clear();
    error_code e = any(doc);
    if (e) return e;
    *end = compact(begin_, out_, headers_);
    return SUCCESS;
  }

 private:
  // A value, of any type: a document, a document_reference or a value.
  template <typename V>
  simdjson_inline error_code any(V& v) {
    ondemand::json_type t;
    SIMDJSON_TRY(v.type().get(t));
    switch (t) {
      case ondemand::json_type::array: {
        ondemand::array a;
        SIMDJSON_TRY(v.get_array().get(a));
        return array(a);
      }
      case ondemand::json_type::object: {
        ondemand::object o;
        SIMDJSON_TRY(v.get_object().get(o));
        return object(o);
      }
      default:
        return scalar(v, t);
    }
  }

  template <typename V>
  simdjson_inline error_code scalar(V& v, ondemand::json_type t) {
    switch (t) {
      case ondemand::json_type::number:
        return number(v);
      case ondemand::json_type::string: {
        ondemand::raw_json_string s;
        SIMDJSON_TRY(v.get_raw_json_string().get(s));
        return string(s);
      }
      case ondemand::json_type::boolean: {
        bool b;
        SIMDJSON_TRY(v.get_bool().get(b));
        *out_++ = uint8_t(0xc2 + b);
        return SUCCESS;
      }
      case ondemand::json_type::null: {
        bool is_null;
        SIMDJSON_TRY(v.is_null().get(is_null));
        if (!is_null) return N_ATOM_ERROR;
        *out_++ = 0xc0;
        return SUCCESS;
      }
      default:
        return TAPE_ERROR;
    }
  }

  // An element of an array or the value of a field: scalars here, containers
  // in a call (most values are scalars).
  simdjson_inline error_code element(ondemand::value& v) {
    ondemand::json_type t;
    SIMDJSON_TRY(v.type().get(t));
    if (t == ondemand::json_type::array || t == ondemand::json_type::object) return container(v);
    return scalar(v, t);
  }
  simdjson_never_inline error_code container(ondemand::value& v) { return any(v); }

  error_code array(ondemand::array& a) {
    const size_t h = open(false);
    uint64_t count = 0;
    for (auto child : a) {
      ondemand::value v;
      SIMDJSON_TRY(child.get(v));
      SIMDJSON_TRY(element(v));
      count++;
    }
    return close(h, count);
  }

  error_code object(ondemand::object& o) {
    const size_t h = open(true);
    uint64_t count = 0;
    for (auto f : o) {
      ondemand::field field;
      SIMDJSON_TRY(std::move(f).get(field));
      SIMDJSON_TRY(string(field.key()));
      SIMDJSON_TRY(element(field.value()));
      count++;
    }
    return close(h, count);
  }

  simdjson_inline size_t open(bool is_map) {
    const size_t h = headers_.size();
    headers_.push_back(header{size_t(out_ - begin_), 0, is_map});
    out_ += header_room;
    return h;
  }
  simdjson_inline error_code close(size_t h, uint64_t count) {
    if (count > std::numeric_limits<uint32_t>::max()) return CAPACITY;
    headers_[h].count = uint32_t(count);
    return SUCCESS;
  }

  template <typename V>
  simdjson_inline error_code number(V& v) {
    ondemand::number n;
    error_code e = v.get_number().get(n);
    if (simdjson_unlikely(e)) {
      if (e != BIGINT_ERROR || !options_.big_integers_as_float64) return e;
      double d;
      SIMDJSON_TRY(v.get_double().get(d));
      put_double(d);
      return SUCCESS;
    }
    switch (n.get_number_type()) {
      case ondemand::number_type::signed_integer:
        put_int(n.get_int64());
        break;
      case ondemand::number_type::unsigned_integer:
        put_uint(n.get_uint64());
        break;
      default:
        put_double(n.get_double());
    }
    return SUCCESS;
  }

  simdjson_inline void put_uint(uint64_t x) {
    if (x < 128) {
      *out_++ = uint8_t(x);
    } else if (x <= 0xff) {
      out_[0] = 0xcc;
      out_[1] = uint8_t(x);
      out_ += 2;
    } else if (x <= 0xffff) {
      *out_ = 0xcd;
      out_ = put16(out_ + 1, uint16_t(x));
    } else if (x <= 0xffffffff) {
      *out_ = 0xce;
      out_ = put32(out_ + 1, uint32_t(x));
    } else {
      *out_ = 0xcf;
      out_ = put64(out_ + 1, x);
    }
  }
  simdjson_inline void put_int(int64_t x) {
    if (x >= 0) return put_uint(uint64_t(x));
    if (x >= -32) {
      *out_++ = uint8_t(x);  // negative fixint: 0xe0 to 0xff
    } else if (x >= -128) {
      out_[0] = 0xd0;
      out_[1] = uint8_t(x);
      out_ += 2;
    } else if (x >= -32768) {
      *out_ = 0xd1;
      out_ = put16(out_ + 1, uint16_t(x));
    } else if (x >= -2147483648LL) {
      *out_ = 0xd2;
      out_ = put32(out_ + 1, uint32_t(x));
    } else {
      *out_ = 0xd3;
      out_ = put64(out_ + 1, uint64_t(x));
    }
  }
  simdjson_inline void put_double(double d) {
    if (options_.float32_when_exact) {
      const float f = float(d);
      if (double(f) == d) {
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        *out_ = 0xca;
        out_ = put32(out_ + 1, bits);
        return;
      }
    }
    uint64_t bits;
    std::memcpy(&bits, &d, 8);
    *out_ = 0xcb;
    out_ = put64(out_ + 1, bits);
  }

  // Unescapes the string after one byte (a fixstr header), then moves it
  // forward if it needs a longer header.
  simdjson_inline error_code string(ondemand::raw_json_string s) {
    uint8_t* const at = out_;
    uint8_t* end = at + 1;
    std::string_view v;
    SIMDJSON_TRY(parser_.unescape(s, end).get(v));
    const size_t n = v.size();
    if (simdjson_likely(n < 32)) {
      *at = uint8_t(0xa0 | n);
    } else if (n <= 0xff) {
      std::memmove(at + 2, at + 1, n);
      at[0] = 0xd9;
      at[1] = uint8_t(n);
      end += 1;
    } else if (n <= 0xffff) {
      std::memmove(at + 3, at + 1, n);
      at[0] = 0xda;
      put16(at + 1, uint16_t(n));
      end += 2;
    } else {
      if (n > std::numeric_limits<uint32_t>::max()) return CAPACITY;
      std::memmove(at + 5, at + 1, n);
      at[0] = 0xdb;
      put32(at + 1, uint32_t(n));
      end += 4;
    }
    out_ = end;
    return SUCCESS;
  }

  ondemand::parser& parser_;
  std::vector<header>& headers_;
  const msgpack_options& options_;
  uint8_t* begin_{nullptr};
  uint8_t* out_{nullptr};
};

std::string error_text(error_code e, size_t document) {
  std::string message = error_message(e);
  if (document != size_t(-1)) message += " (document " + std::to_string(document) + ")";
  return message;
}

// The documents of newline-delimited JSON, converted into out by one thread.
struct ndjson_result {
  error_code error{SUCCESS};
  size_t count{0};  // documents written, or the one that failed
};

}  // namespace

struct msgpack_writer::impl {
  impl(const msgpack_options& o, const allocator* a) : options(o), out(a) {}

  msgpack_options options;
  internal::buffer out;
  ondemand::parser parser;
  std::vector<header> headers;

  // Writes the documents of json into into; on error, into keeps what it had
  // written (the caller truncates it).
  ndjson_result ndjson(padded_string_view json, ondemand::parser& p, std::vector<header>& h,
                       internal::buffer& into) {
    ndjson_result r;
    size_t batch = std::max<size_t>(options.block_size, size_t(1) << 16);
    for (;;) {
      ondemand::document_stream stream;
      r.error = p.iterate_many(json.data(), json.size(), std::min(batch, json.size()))
                    .get(stream);
      if (r.error) return r;
      encoder enc(p, h, options);
      r.count = 0;
      const size_t start = into.size();
      for (auto it = stream.begin(); it != stream.end(); ++it) {
        ondemand::document_reference doc;
        r.error = (*it).get(doc);
        if (!r.error) {
          // The document's text bounds what we write.
          const size_t room = max_msgpack_size(it.source().size());
          uint8_t* at = reinterpret_cast<uint8_t*>(into.reserve(room));
          uint8_t* end;
          r.error = enc.write(doc, at, &end);
          if (!r.error) into.resize(size_t(reinterpret_cast<char*>(end) - into.data()));
        }
        if (r.error) break;
        r.count++;
      }
      // A document larger than the batch: once more, with the whole input.
      if (r.error == CAPACITY && batch < json.size()) {
        into.resize(start);
        batch = json.size();
        continue;
      }
      if (!r.error && stream.truncated_bytes() != 0) r.error = TAPE_ERROR;
      return r;
    }
  }

  status write_ndjson(padded_string_view json, size_t* count) {
    const size_t start = out.size();
    try {
      if (options.threads > 1 && json.size() > options.block_size) {
        if (parallel_ndjson(json, count)) return status{};
        out.resize(start);
      }
      ndjson_result r = ndjson(json, parser, headers, out);
      if (r.error) {
        out.resize(start);
        return status{error_text(r.error, r.count)};
      }
      if (count != nullptr) *count += r.count;
      return status{};
    } catch (const std::bad_alloc&) {
      out.resize(start);
      return status{"out of memory"};
    }
  }

  // What each thread keeps between calls.
  struct worker {
    ondemand::parser parser;
    std::vector<header> headers;
    internal::buffer out;
  };
  std::vector<std::unique_ptr<worker>> workers;

  // Converts blocks of about block_size bytes, cut at line ends, on several
  // threads. Returns false if one fails (to convert again in one thread).
  bool parallel_ndjson(padded_string_view json, size_t* count) {
    const size_t block = std::max<size_t>(options.block_size, 1);
    const size_t n = (json.size() + block - 1) / block;
    const char* data = json.data();
    const size_t length = json.size();
    // Block i starts after the first line end at or after i * block.
    auto block_start = [&](size_t i) -> size_t {
      if (i == 0) return 0;
      if (i >= n) return length;
      const void* nl = std::memchr(data + i * block, '\n', length - i * block);
      return nl == nullptr ? length : size_t(static_cast<const char*>(nl) - data) + 1;
    };
    const size_t threads = std::min(options.threads, n);
    if (workers.size() < threads) workers.resize(threads);
    for (auto& w : workers) {
      if (!w) w.reset(new worker);
      w->out.resize(0);
    }
    std::vector<internal::block_output> blocks(n);
    std::vector<size_t> counts(n, 0);
    std::atomic<bool> failed{false};
    internal::parallel_for(n, threads, [&](size_t i, size_t w) {
      if (failed) return;
      const size_t begin = block_start(i);
      const size_t end = std::max(begin, block_start(i + 1));
      if (begin == end) return;
      worker& me = *workers[w];
      const size_t offset = me.out.size();
      padded_string_view part(data + begin, end - begin, json.capacity() - begin);
      ndjson_result r = ndjson(part, me.parser, me.headers, me.out);
      if (r.error) failed = true;
      counts[i] = r.count;
      blocks[i] = internal::block_output{w, offset, me.out.size() - offset};
    });
    if (failed) return false;
    std::vector<internal::buffer*> buffers;
    for (auto& w : workers) buffers.push_back(&w->out);
    internal::gather(out, buffers, blocks, threads);
    size_t documents = 0;
    for (size_t c : counts) documents += c;
    if (count != nullptr) *count += documents;
    return true;
  }
};

msgpack_writer::msgpack_writer(const msgpack_options& options)
    : impl_(new impl(options, nullptr)) {}
msgpack_writer::msgpack_writer(const msgpack_options& options, const allocator& a)
    : impl_(new impl(options, &a)) {}
msgpack_writer::~msgpack_writer() = default;

status msgpack_writer::write_json(padded_string_view json) {
  impl& m = *impl_;
  const size_t start = m.out.size();
  try {
    uint8_t* at = reinterpret_cast<uint8_t*>(m.out.reserve(max_msgpack_size(json.size())));
    ondemand::document doc;
    error_code e = m.parser.iterate(json).get(doc);
    uint8_t* end = at;
    encoder enc(m.parser, m.headers, m.options);
    if (!e) e = enc.write(doc, at, &end);
    if (!e && !doc.at_end()) e = TRAILING_CONTENT;
    if (e) return status{error_text(e, size_t(-1))};
    m.out.resize(size_t(reinterpret_cast<char*>(end) - m.out.data()));
    return status{};
  } catch (const std::bad_alloc&) {
    m.out.resize(start);
    return status{"out of memory"};
  }
}

status msgpack_writer::write_json(const char* json, size_t length) {
  try {
    padded_string copy(json, length);
    return write_json(padded_string_view(copy));
  } catch (const std::bad_alloc&) {
    return status{"out of memory"};
  }
}

status msgpack_writer::write_ndjson(padded_string_view json, size_t* count) {
  return impl_->write_ndjson(json, count);
}

status msgpack_writer::write_ndjson(const char* json, size_t length, size_t* count) {
  try {
    padded_string copy(json, length);
    return write_ndjson(padded_string_view(copy), count);
  } catch (const std::bad_alloc&) {
    return status{"out of memory"};
  }
}

std::string_view msgpack_writer::view() const { return impl_->out.view(); }
char* msgpack_writer::release(size_t* size) { return impl_->out.release(size); }
void msgpack_writer::clear() { impl_->out.resize(0); }

namespace {
template <typename F>
status append_to(std::string* out, const msgpack_options& options, F write) {
  msgpack_writer writer(options);
  status s = write(writer);
  if (!s.ok()) return s;
  try {
    out->append(writer.view());
  } catch (const std::bad_alloc&) {
    return status{"out of memory"};
  }
  return s;
}
}  // namespace

status json_to_msgpack(padded_string_view json, std::string* out, const msgpack_options& options) {
  return append_to(out, options, [&](msgpack_writer& w) { return w.write_json(json); });
}

status ndjson_to_msgpack(padded_string_view json, std::string* out,
                         const msgpack_options& options) {
  return append_to(out, options, [&](msgpack_writer& w) { return w.write_ndjson(json); });
}

}  // namespace simdjson_msgpack
