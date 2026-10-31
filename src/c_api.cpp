// The C entry points.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <new>

#include "simdjson_msgpack.h"

namespace {

void set_error(const std::string& message, char* error, size_t capacity) {
  if (error == nullptr || capacity == 0) return;
  const size_t n = std::min(message.size(), capacity - 1);
  std::memcpy(error, message.data(), n);
  error[n] = '\0';
}

}  // namespace

extern "C" {

int simdjson_msgpack_json_to_msgpack(const char* data, size_t length, size_t capacity,
                                     int ndjson, size_t threads, char** out,
                                     size_t* out_length, char* error, size_t error_capacity) {
  using namespace simdjson_msgpack;
  try {
    msgpack_options options;
    if (threads != 0) options.threads = threads;
    msgpack_writer writer(options);
    status s;
    if (capacity >= length + simdjson::SIMDJSON_PADDING) {
      simdjson::padded_string_view json(data, length, capacity);
      s = ndjson ? writer.write_ndjson(json) : writer.write_json(json);
    } else {
      s = ndjson ? writer.write_ndjson(data, length) : writer.write_json(data, length);
    }
    if (!s.ok()) {
      set_error(s.message, error, error_capacity);
      return 1;
    }
    *out = writer.release(out_length);
    return 0;
  } catch (const std::bad_alloc&) {
    set_error("out of memory", error, error_capacity);
    return 1;
  }
}

int simdjson_msgpack_msgpack_to_json(const char* data, size_t length, int stream,
                                     int validate_utf8, int binary_as_base64, int ext_as_null,
                                     size_t threads, char** out, size_t* out_length, char* error,
                                     size_t error_capacity) {
  using namespace simdjson_msgpack;
  try {
    json_options options;
    options.validate_utf8 = validate_utf8 != 0;
    options.binary_as_base64 = binary_as_base64 != 0;
    options.ext = ext_as_null ? ext_policy::null : ext_policy::error;
    if (threads != 0) options.threads = threads;
    json_writer writer(options);
    status s = stream ? writer.write_msgpack_stream(data, length)
                      : writer.write_msgpack(data, length);
    if (!s.ok()) {
      set_error(s.message, error, error_capacity);
      return 1;
    }
    *out = writer.release(out_length);
    return 0;
  } catch (const std::bad_alloc&) {
    set_error("out of memory", error, error_capacity);
    return 1;
  }
}

void simdjson_msgpack_free(void* p) { std::free(p); }

}  // extern "C"
