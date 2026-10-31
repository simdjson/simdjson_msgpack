// JSON to MessagePack and back.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "simdjson_msgpack.h"

using namespace simdjson_msgpack;

int main() {
  simdjson::padded_string json(std::string(R"({"id":1,"name":"Ada","scores":[9.5,7],"admin":false})"));

  // One document.
  std::string msgpack;
  status s = json_to_msgpack(json, &msgpack);
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.message.c_str());
    return EXIT_FAILURE;
  }
  std::printf("%zu bytes of JSON, %zu bytes of MessagePack\n", json.size(), msgpack.size());

  std::string back;
  s = msgpack_to_json(msgpack.data(), msgpack.size(), &back);
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.message.c_str());
    return EXIT_FAILURE;
  }
  std::printf("%s\n", back.c_str());

  // Many documents: keep the writers, which keep their buffers.
  simdjson::padded_string ndjson(std::string("{\"a\":1}\n{\"a\":2}\n[true,null]\n"));
  msgpack_writer writer;
  size_t count = 0;
  s = writer.write_ndjson(ndjson, &count);
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.message.c_str());
    return EXIT_FAILURE;
  }
  json_writer jwriter;
  s = jwriter.write_msgpack_stream(writer.view().data(), writer.view().size());
  if (!s.ok()) {
    std::fprintf(stderr, "%s\n", s.message.c_str());
    return EXIT_FAILURE;
  }
  std::printf("%zu documents:\n%.*s", count, int(jwriter.view().size()), jwriter.view().data());
  return back == std::string_view(json) ? EXIT_SUCCESS : EXIT_FAILURE;
}
