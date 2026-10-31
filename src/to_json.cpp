// Converts MessagePack to JSON.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "simdjson_msgpack.h"

#include "buffer.h"
#include "parallel.h"
#include "zmij.h"  // shortest representation of floating-point numbers

namespace simdjson_msgpack {
namespace {

simdjson_inline uint16_t get16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
simdjson_inline uint32_t get32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
simdjson_inline uint64_t get64(const uint8_t* p) {
  return (uint64_t(get32(p)) << 32) | get32(p + 4);
}

constexpr uint64_t ones = 0x0101010101010101;
constexpr uint64_t highs = 0x8080808080808080;

// Whether one of the 8 bytes is a control character, a quote or a backslash.
simdjson_inline bool needs_escape(uint64_t word) {
  auto has_zero = [](uint64_t v) { return (v - ones) & ~v & highs; };
  const uint64_t control = (word - 0x20 * ones) & ~word & highs;  // a byte < 0x20
  return (control | has_zero(word ^ ('"' * ones)) | has_zero(word ^ ('\\' * ones))) != 0;
}
simdjson_inline bool needs_escape(unsigned char c) { return c < 0x20 || c == '"' || c == '\\'; }

char* escape(char* out, unsigned char c) {
  *out++ = '\\';
  switch (c) {
    case '"':
      *out++ = '"';
      break;
    case '\\':
      *out++ = '\\';
      break;
    case '\b':
      *out++ = 'b';
      break;
    case '\f':
      *out++ = 'f';
      break;
    case '\n':
      *out++ = 'n';
      break;
    case '\r':
      *out++ = 'r';
      break;
    case '\t':
      *out++ = 't';
      break;
    default: {
      static const char hex[] = "0123456789abcdef";
      std::memcpy(out, "u00", 3);
      out[3] = hex[c >> 4];
      out[4] = hex[c & 15];
      out += 5;
    }
  }
  return out;
}

// Writes in[0, n) between quotes, escaped; sets *non_ascii if a byte is not
// ASCII. out must have room for 2 + 6 * n bytes.
simdjson_inline char* escape_with_quotes(char* out, const char* in, size_t n, bool* non_ascii) {
  *out++ = '"';
  const char* const end = in + n;
  uint64_t seen = 0;
  while (in != end) {
    // Copy the bytes that need no escaping, 8 at a time.
    while (end - in >= 8) {
      uint64_t word;
      std::memcpy(&word, in, 8);
      if (needs_escape(word)) break;
      seen |= word;
      std::memcpy(out, in, 8);
      in += 8;
      out += 8;
    }
    while (in != end && !needs_escape(static_cast<unsigned char>(*in))) {
      seen |= static_cast<unsigned char>(*in);
      *out++ = *in++;
    }
    if (in == end) break;
    out = escape(out, static_cast<unsigned char>(*in++));
  }
  *out++ = '"';
  *non_ascii = (seen & highs) != 0;
  return out;
}

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define SIMDJSON_MSGPACK_SHORT_STRINGS 0
#else
#define SIMDJSON_MSGPACK_SHORT_STRINGS 1
#endif

#if SIMDJSON_MSGPACK_SHORT_STRINGS
// Bytes that need escaping (or are not ASCII), as their high bits. A byte
// may be flagged wrongly only above (at a higher address than) a byte that
// is rightly flagged.
simdjson_inline uint64_t flags(uint64_t word) {
  auto has_zero = [](uint64_t v) { return (v - ones) & ~v; };
  const uint64_t control = (word - 0x20 * ones) & ~word;  // a byte < 0x20
  return (control | has_zero(word ^ ('"' * ones)) | has_zero(word ^ ('\\' * ones)) | word) & highs;
}

// A string of at most 16 bytes of ASCII that needs no escaping, between
// quotes: we may read 16 bytes at in and write 18 at out. Returns null if a
// byte needs escaping or is not ASCII (little endian only).
simdjson_inline char* short_string(char* out, const char* in, size_t n) {
  uint64_t w0, w1;
  std::memcpy(&w0, in, 8);
  std::memcpy(&w1, in + 8, 8);
  // The high bits of the bytes below n.
  const uint64_t m0 = n >= 8 ? highs : highs & ((uint64_t(1) << (8 * n)) - 1);
  const uint64_t m1 = n >= 16 ? highs : n <= 8 ? 0 : highs & ((uint64_t(1) << (8 * (n - 8))) - 1);
  if (((flags(w0) & m0) | (flags(w1) & m1)) != 0) return nullptr;
  out[0] = '"';
  std::memcpy(out + 1, in, 16);
  out[1 + n] = '"';
  return out + 2 + n;
}
#endif

// Base64 (standard alphabet, with padding) of in[0, n), between quotes. out
// must have room for 2 + 4 * ((n + 2) / 3) bytes.
char* base64_with_quotes(char* out, const uint8_t* in, size_t n) {
  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  *out++ = '"';
  size_t i = 0;
  for (; i + 3 <= n; i += 3) {
    const uint32_t v = (uint32_t(in[i]) << 16) | (uint32_t(in[i + 1]) << 8) | in[i + 2];
    out[0] = table[v >> 18];
    out[1] = table[(v >> 12) & 63];
    out[2] = table[(v >> 6) & 63];
    out[3] = table[v & 63];
    out += 4;
  }
  if (i < n) {
    const uint32_t v = (uint32_t(in[i]) << 16) | (i + 1 < n ? uint32_t(in[i + 1]) << 8 : 0);
    out[0] = table[v >> 18];
    out[1] = table[(v >> 12) & 63];
    out[2] = i + 1 < n ? table[(v >> 6) & 63] : '=';
    out[3] = '=';
    out += 4;
  }
  *out++ = '"';
  return out;
}

// Writes MessagePack values as JSON, at the end of a buffer.
class decoder {
 public:
  decoder(internal::buffer& out, const json_options& options, const uint8_t* begin,
          const uint8_t* end)
      : buffer_(out), options_(options), begin_(begin), in_(begin), end_(end) {
    out_ = limit_ = out.data() + out.size();
  }

  // Writes the next value; returns false on error (see message()).
  bool value() {
    room(element_room);
    bool ok = any(0);
    sync();
    return ok;
  }
  bool at_end() const { return in_ == end_; }
  void put(char c) {
    room(1);
    *out_++ = c;
    sync();
  }
  const uint8_t* position() const { return in_; }
  std::string message() const {
    return error_ + " (byte " + std::to_string(size_t(error_at_ - begin_)) + ")";
  }

 private:
  // We check the room left in the output once per element of an array or a
  // map (not for every token): element_room bytes hold a comma, a scalar key
  // between quotes, a colon and a scalar value (at most 1 + 42 + 1 + 40).
  // Strings and binary values make room for themselves and after_room more
  // bytes (for the colon and the value that may follow a key).
  static constexpr size_t element_room = 96;
  static constexpr size_t after_room = 48;

  // Makes room for n bytes of output.
  simdjson_inline void room(size_t n) {
    if (simdjson_unlikely(size_t(limit_ - out_) < n)) {
      buffer_.resize(size_t(out_ - buffer_.data()));
      out_ = buffer_.reserve(n);
      limit_ = buffer_.data() + buffer_.capacity();
    }
  }
  void sync() { buffer_.resize(size_t(out_ - buffer_.data())); }

  bool fail(const char* message, const uint8_t* at) {
    error_ = message;
    error_at_ = at;
    return false;
  }
  // Whether n more bytes of input follow.
  simdjson_inline bool has(size_t n) const { return size_t(end_ - in_) >= n; }

  simdjson_inline void put_uint(uint64_t x) {
    out_ = simdjson::internal::write_uint_jeaiii(out_, x);
  }
  simdjson_inline void put_int(int64_t x) {
    const bool negative = x < 0;
    *out_ = '-';
    out_ += negative;
    const uint64_t magnitude = negative ? 0 - static_cast<uint64_t>(x) : static_cast<uint64_t>(x);
    out_ = simdjson::internal::write_uint_jeaiii(out_, magnitude);
  }
  // The shortest representation that gives back x, with a decimal point or
  // an exponent (1.0, not 1), so that it reads back as a floating-point
  // number. NaN and infinities become null.
  template <typename F>
  simdjson_inline void put_float(F x) {
    constexpr size_t n = 40;  // zmij needs at most 34 bytes, ".0" 2 more
    // element_room covers it, but GCC writes floats 10% to 15% faster with
    // the check (its code for zmij comes out otherwise).
    room(n);
    if (!std::isfinite(x)) {
      std::memcpy(out_, "null", 4);
      out_ += 4;
      return;
    }
    char* end = zmij::write(out_, n, x);
    // zmij writes integers without a decimal point in fixed notation: below
    // 1e16 for a double (as Python's repr). For a float, we look for a
    // decimal point or an exponent.
    bool integer;
    SIMDJSON_IF_CONSTEXPR(sizeof(F) == 8) {
      integer = x == std::floor(x) && std::fabs(x) < 1e16;
    } else {
      integer = std::find_if(out_, end, [](char c) { return c == '.' || c == 'e'; }) == end;
    }
    if (integer) {
      std::memcpy(end, ".0", 2);
      end += 2;
    }
    out_ = end;
  }
  simdjson_inline void put_raw(const char* s, size_t n) {
    std::memcpy(out_, s, n);
    out_ += n;
  }

  simdjson_inline bool string(const uint8_t* at, size_t n) {
    if (simdjson_unlikely(!has(n))) return fail("truncated string", at);
    const char* s = reinterpret_cast<const char*>(in_);
    room(18 + 6 * n + after_room);
#if SIMDJSON_MSGPACK_SHORT_STRINGS
    // Most strings (keys) are short, ASCII and need no escaping.
    if (n <= 16 && size_t(end_ - in_) >= 16) {
      char* end = short_string(out_, s, n);
      if (simdjson_likely(end != nullptr)) {
        out_ = end;
        in_ += n;
        return true;
      }
    }
#endif
    bool non_ascii;
    out_ = escape_with_quotes(out_, s, n, &non_ascii);
    if (non_ascii && options_.validate_utf8 && !simdjson::validate_utf8(s, n)) {
      return fail("invalid UTF-8 in a string", at);
    }
    in_ += n;
    return true;
  }
  bool binary(const uint8_t* at, size_t n) {
    if (!options_.binary_as_base64) return fail("binary value (binary_as_base64 is off)", at);
    if (!has(n)) return fail("truncated binary value", at);
    room(2 + 4 * ((n + 2) / 3) + after_room);
    out_ = base64_with_quotes(out_, in_, n);
    in_ += n;
    return true;
  }
  // An extension value: its type at in_, then n bytes.
  bool ext(const uint8_t* at, size_t n) {
    if (!has(1 + n)) return fail("truncated extension value", at);
    if (options_.ext != ext_policy::null) return fail("extension value", at);
    in_ += 1 + n;
    put_raw("null", 4);
    return true;
  }

  bool array(const uint8_t* at, size_t n, size_t depth) {
    if (depth >= options_.max_depth) return fail("nested too deeply", at);
    *out_++ = '[';
    for (size_t i = 0; i < n; i++) {
      room(element_room);
      if (i != 0) *out_++ = ',';
      if (!any(depth + 1)) return false;
    }
    room(1);
    *out_++ = ']';
    return true;
  }
  bool map(const uint8_t* at, size_t n, size_t depth) {
    if (depth >= options_.max_depth) return fail("nested too deeply", at);
    *out_++ = '{';
    for (size_t i = 0; i < n; i++) {
      room(element_room);
      if (i != 0) *out_++ = ',';
      if (!key()) return false;
      *out_++ = ':';
      if (!any(depth + 1)) return false;
    }
    room(1);
    *out_++ = '}';
    return true;
  }

  // A map key: a string, or a scalar that we write between quotes.
  simdjson_inline bool key() {
    if (simdjson_unlikely(!has(1))) return fail("truncated map", in_);
    const uint8_t b = *in_;
    if (simdjson_likely((b & 0xe0) == 0xa0)) {  // fixstr
      const uint8_t* at = in_++;
      return string(at, b & 0x1f);
    }
    switch (b) {
      case 0xd9:
      case 0xda:
      case 0xdb:  // str
      case 0xc4:
      case 0xc5:
      case 0xc6:  // bin, as base64
        return any(0);
      default:
        break;
    }
    if ((b & 0xf0) == 0x80 || (b & 0xf0) == 0x90 || b == 0xdc || b == 0xdd || b == 0xde ||
        b == 0xdf || (b >= 0xc7 && b <= 0xc9) || (b >= 0xd4 && b <= 0xd8)) {
      return fail("map key that is an array, a map or an extension value", in_);
    }
    put_raw("\"", 1);
    if (!any(0)) return false;
    put_raw("\"", 1);
    return true;
  }

  bool any(size_t depth) {
    if (simdjson_unlikely(!has(1))) return fail("truncated input", in_);
    const uint8_t* const at = in_;
    const uint8_t b = *in_++;
    if (b <= 0x7f) {
      put_uint(b);
      return true;
    }
    if (b >= 0xe0) {
      put_int(int8_t(b));
      return true;
    }
    if (b <= 0x8f) return map(at, b & 0x0f, depth);
    if (b <= 0x9f) return array(at, b & 0x0f, depth);
    if (b <= 0xbf) return string(at, b & 0x1f);
    // Fixed sizes: the bytes that follow the type byte, from 0xc0 to 0xdf.
    static const uint8_t follow[32] = {
        0, 0, 0, 0, 1, 2, 4, 1, 2, 4, 4, 8, 1, 2, 4, 8,  // 0xc0 - 0xcf
        1, 2, 4, 8, 1, 1, 1, 1, 1, 1, 2, 4, 2, 4, 2, 4,  // 0xd0 - 0xdf
    };
    if (simdjson_unlikely(!has(follow[b - 0xc0]))) return fail("truncated input", at);
    const uint8_t* p = in_;
    in_ += follow[b - 0xc0];
    switch (b) {
      case 0xc0:
        put_raw("null", 4);
        return true;
      case 0xc2:
        put_raw("false", 5);
        return true;
      case 0xc3:
        put_raw("true", 4);
        return true;
      case 0xc4:
        return binary(at, p[0]);
      case 0xc5:
        return binary(at, get16(p));
      case 0xc6:
        return binary(at, get32(p));
      case 0xc7:
        return ext(at, p[0]);
      case 0xc8:
        return ext(at, get16(p));
      case 0xc9:
        return ext(at, get32(p));
      case 0xca: {
        const uint32_t bits = get32(p);
        float f;
        std::memcpy(&f, &bits, 4);
        put_float(f);
        return true;
      }
      case 0xcb: {
        const uint64_t bits = get64(p);
        double d;
        std::memcpy(&d, &bits, 8);
        put_float(d);
        return true;
      }
      case 0xcc:
        put_uint(p[0]);
        return true;
      case 0xcd:
        put_uint(get16(p));
        return true;
      case 0xce:
        put_uint(get32(p));
        return true;
      case 0xcf:
        put_uint(get64(p));
        return true;
      case 0xd0:
        put_int(int8_t(p[0]));
        return true;
      case 0xd1:
        put_int(int16_t(get16(p)));
        return true;
      case 0xd2:
        put_int(int32_t(get32(p)));
        return true;
      case 0xd3:
        put_int(int64_t(get64(p)));
        return true;
      case 0xd4:
      case 0xd5:
      case 0xd6:
      case 0xd7:
      case 0xd8:  // fixext 1, 2, 4, 8, 16: we have read the type
        in_ = p;
        return ext(at, size_t(1) << (b - 0xd4));
      case 0xd9:
        return string(at, p[0]);
      case 0xda:
        return string(at, get16(p));
      case 0xdb:
        return string(at, get32(p));
      case 0xdc:
        return array(at, get16(p), depth);
      case 0xdd:
        return array(at, get32(p), depth);
      case 0xde:
        return map(at, get16(p), depth);
      case 0xdf:
        return map(at, get32(p), depth);
      default:  // 0xc1
        return fail("invalid byte 0xc1", at);
    }
  }

  internal::buffer& buffer_;
  const json_options& options_;
  const uint8_t* const begin_;
  const uint8_t* in_;
  const uint8_t* const end_;
  char* out_;
  char* limit_;
  std::string error_;
  const uint8_t* error_at_{nullptr};
};

// Returns the end of the value at p, or null if it is not whole (we do not
// check it otherwise). Iterative: no recursion, whatever the depth.
const uint8_t* skip(const uint8_t* p, const uint8_t* end) {
  // Fixed sizes: the bytes that follow the type byte, from 0xc0 to 0xdf.
  static const uint8_t follow[32] = {
      0, 0, 0, 0, 1, 2, 4, 1, 2, 4, 4, 8, 1, 2, 4, 8,
      1, 2, 4, 8, 2, 3, 5, 9, 17, 1, 2, 4, 2, 4, 2, 4,
  };
  uint64_t pending = 1;  // values still to skip
  while (pending != 0) {
    if (p == end) return nullptr;
    const uint8_t b = *p++;
    pending--;
    if (b <= 0x7f || b >= 0xe0) continue;
    if (b <= 0x8f) {
      pending += 2 * uint64_t(b & 0x0f);
      continue;
    }
    if (b <= 0x9f) {
      pending += b & 0x0f;
      continue;
    }
    uint64_t length;
    if (b <= 0xbf) {
      length = b & 0x1f;
    } else {
      const size_t f = follow[b - 0xc0];
      if (size_t(end - p) < f) return nullptr;
      const uint8_t* q = p;
      p += f;
      switch (b) {
        case 0xc4:
        case 0xd9:
          length = q[0];
          break;
        case 0xc5:
        case 0xda:
          length = get16(q);
          break;
        case 0xc6:
        case 0xdb:
          length = get32(q);
          break;
        case 0xc7:
          length = uint64_t(q[0]) + 1;  // and the type
          break;
        case 0xc8:
          length = uint64_t(get16(q)) + 1;
          break;
        case 0xc9:
          length = uint64_t(get32(q)) + 1;
          break;
        case 0xdc:
          pending += get16(q);
          continue;
        case 0xdd:
          pending += get32(q);
          continue;
        case 0xde:
          pending += 2 * uint64_t(get16(q));
          continue;
        case 0xdf:
          pending += 2 * uint64_t(get32(q));
          continue;
        case 0xc1:
          return nullptr;
        default:  // numbers, nil, booleans, fixext: all in follow
          continue;
      }
    }
    if (uint64_t(end - p) < length) return nullptr;
    p += length;
  }
  return p;
}

}  // namespace

struct json_writer::impl {
  impl(const json_options& o, const allocator* a) : options(o), out(a) {}
  json_options options;
  internal::buffer out;
  std::vector<std::unique_ptr<internal::buffer>> workers;  // kept between calls

  // Writes the values of p[0, length) as newline-delimited JSON at the end of
  // into; returns an empty string, or the error.
  std::string stream(const uint8_t* p, size_t length, internal::buffer& into, size_t* count) {
    decoder d(into, options, p, p + length);
    size_t n = 0;
    while (!d.at_end()) {
      if (!d.value()) return d.message();
      d.put('\n');
      n++;
    }
    *count = n;
    return std::string();
  }

  // Writes blocks of whole values of about block_size bytes on several
  // threads. The calling thread finds where the blocks end (skipping over
  // the values), and the others write the blocks as soon as they are known;
  // then the calling thread writes blocks too. Returns false if that fails
  // (to write again in one thread).
  bool parallel_stream(const uint8_t* p, size_t length, size_t* count) {
    const uint8_t* const end = p + length;
    const size_t block = std::max<size_t>(options.block_size, 1);
    const size_t max_blocks = length / block + 1;
    // Block i is [starts[i], starts[i + 1]); found blocks are known.
    std::vector<size_t> starts(max_blocks + 1, 0);
    std::atomic<size_t> found{0};
    enum { finding, found_all, failed };
    std::atomic<int> state{finding};
    const size_t threads = std::min(options.threads, max_blocks);
    if (workers.size() < threads) workers.resize(threads);
    for (auto& w : workers) {
      if (!w) w.reset(new internal::buffer);
      w->resize(0);
    }
    std::vector<internal::block_output> blocks(max_blocks);
    std::vector<size_t> counts(max_blocks, 0);
    std::atomic<size_t> next_block{0};

    auto find_blocks = [&] {
      const uint8_t* q = p;
      size_t n = 0;
      while (q != end) {
        const uint8_t* limit = q + std::min(block, size_t(end - q));
        while (q < limit) {
          q = skip(q, end);
          if (q == nullptr) {
            state.store(failed, std::memory_order_release);
            return;
          }
        }
        starts[++n] = size_t(q - p);
        found.store(n, std::memory_order_release);
      }
      state.store(found_all, std::memory_order_release);
    };
    auto write_blocks = [&](size_t w) {
      for (;;) {
        const size_t i = next_block++;
        // Wait for block i to be known.
        for (;;) {
          if (found.load(std::memory_order_acquire) > i) break;
          const int st = state.load(std::memory_order_acquire);
          if (st == failed) return;
          if (st == found_all) {
            if (found.load(std::memory_order_acquire) > i) break;
            return;
          }
          std::this_thread::yield();
        }
        if (state.load(std::memory_order_relaxed) == failed) return;
        internal::buffer& mine = *workers[w];
        const size_t offset = mine.size();
        try {
          if (!stream(p + starts[i], starts[i + 1] - starts[i], mine, &counts[i]).empty()) {
            state.store(failed, std::memory_order_release);
            return;
          }
        } catch (const std::bad_alloc&) {
          state.store(failed, std::memory_order_release);
          return;
        }
        blocks[i] = internal::block_output{w, offset, mine.size() - offset};
      }
    };
    std::vector<std::thread> pool;
    try {
      pool.reserve(threads - 1);
      for (size_t t = 1; t < threads; t++) pool.emplace_back(write_blocks, t);
    } catch (...) {  // std::system_error or std::bad_alloc: fewer threads
    }
    find_blocks();
    write_blocks(0);
    for (std::thread& t : pool) t.join();
    if (state.load() == failed) return false;
    const size_t n = found.load();
    blocks.resize(n);
    std::vector<internal::buffer*> buffers;
    for (auto& w : workers) buffers.push_back(w.get());
    internal::gather(out, buffers, blocks, threads);
    size_t values = 0;
    for (size_t i = 0; i < n; i++) values += counts[i];
    if (count != nullptr) *count += values;
    return true;
  }
};

json_writer::json_writer(const json_options& options) : impl_(new impl(options, nullptr)) {}
json_writer::json_writer(const json_options& options, const allocator& a)
    : impl_(new impl(options, &a)) {}
json_writer::~json_writer() = default;

status json_writer::write_msgpack(const void* data, size_t length) {
  impl& m = *impl_;
  const size_t start = m.out.size();
  const uint8_t* p = static_cast<const uint8_t*>(data);
  try {
    decoder d(m.out, m.options, p, p + length);
    if (!d.value()) {
      m.out.resize(start);
      return status{d.message()};
    }
    if (!d.at_end()) {
      m.out.resize(start);
      return status{"unexpected bytes after the value (byte " +
                    std::to_string(size_t(d.position() - p)) + ")"};
    }
    return status{};
  } catch (const std::bad_alloc&) {
    m.out.resize(start);
    return status{"out of memory"};
  }
}

status json_writer::write_msgpack_stream(const void* data, size_t length, size_t* count) {
  impl& m = *impl_;
  const size_t start = m.out.size();
  const uint8_t* p = static_cast<const uint8_t*>(data);
  try {
    if (m.options.threads > 1 && length > m.options.block_size) {
      if (m.parallel_stream(p, length, count)) return status{};
      m.out.resize(start);
    }
    size_t n = 0;
    std::string error = m.stream(p, length, m.out, &n);
    if (!error.empty()) {
      m.out.resize(start);
      return status{error};
    }
    if (count != nullptr) *count += n;
    return status{};
  } catch (const std::bad_alloc&) {
    m.out.resize(start);
    return status{"out of memory"};
  }
}

std::string_view json_writer::view() const { return impl_->out.view(); }
char* json_writer::release(size_t* size) { return impl_->out.release(size); }
void json_writer::clear() { impl_->out.resize(0); }

status msgpack_to_json(const void* data, size_t length, std::string* out,
                       const json_options& options) {
  json_writer writer(options);
  status s = writer.write_msgpack(data, length);
  if (!s.ok()) return s;
  try {
    out->append(writer.view());
  } catch (const std::bad_alloc&) {
    return status{"out of memory"};
  }
  return s;
}

status msgpack_to_ndjson(const void* data, size_t length, std::string* out,
                         const json_options& options) {
  json_writer writer(options);
  status s = writer.write_msgpack_stream(data, length);
  if (!s.ok()) return s;
  try {
    out->append(writer.view());
  } catch (const std::bad_alloc&) {
    return status{"out of memory"};
  }
  return s;
}

}  // namespace simdjson_msgpack
