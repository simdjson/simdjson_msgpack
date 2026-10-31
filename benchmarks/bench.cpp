// Converts JSON files to MessagePack and back, and reports the best
// throughput over several rounds, in GB/s of JSON (read, or written).
//   bench [--ndjson] [--threads N] [--rounds N] file.json...
// With --ndjson, a file holds newline-delimited JSON (each document becomes
// a MessagePack value, read back as newline-delimited JSON).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "simdjson_msgpack.h"
#include "timing.h"

using namespace simdjson_msgpack;

int main(int argc, char** argv) {
  bool ndjson = false;
  int rounds = 5;
  msgpack_options options;
  json_options joptions;
  std::vector<const char*> files;
  for (int i = 1; i < argc; i++) {
    if (std::strcmp(argv[i], "--ndjson") == 0) {
      ndjson = true;
    } else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
      options.threads = joptions.threads = size_t(std::atoi(argv[++i]));
    } else if (std::strcmp(argv[i], "--rounds") == 0 && i + 1 < argc) {
      rounds = std::atoi(argv[++i]);
    } else {
      files.push_back(argv[i]);
    }
  }
  if (files.empty()) {
    std::fprintf(stderr, "usage: %s [--ndjson] [--threads N] [--rounds N] file.json...\n",
                 argv[0]);
    return EXIT_FAILURE;
  }
  std::printf("%-28s %10s %10s %14s %14s\n", "file", "JSON MB", "msgpack MB", "to msgpack", "to JSON");
  warm_up();
  for (const char* file : files) {
    simdjson::padded_string json;
    if (simdjson::padded_string::load(file).get(json)) {
      std::fprintf(stderr, "cannot load %s\n", file);
      return EXIT_FAILURE;
    }
    msgpack_writer writer(options);
    status s;
    double t1 = best_time(rounds, [&] {
      writer.clear();
      s = ndjson ? writer.write_ndjson(json) : writer.write_json(json);
      return s.ok();
    });
    if (t1 < 0) {
      std::fprintf(stderr, "%s: %s\n", file, s.message.c_str());
      return EXIT_FAILURE;
    }
    const std::string_view mp = writer.view();
    json_writer jwriter(joptions);
    double t2 = best_time(rounds, [&] {
      jwriter.clear();
      s = ndjson ? jwriter.write_msgpack_stream(mp.data(), mp.size())
                 : jwriter.write_msgpack(mp.data(), mp.size());
      return s.ok();
    });
    if (t2 < 0) {
      std::fprintf(stderr, "%s: %s\n", file, s.message.c_str());
      return EXIT_FAILURE;
    }
    const size_t out = jwriter.view().size();
    std::printf("%-28s %10.2f %10.2f %9.2f GB/s %9.2f GB/s\n", basename_of(file).c_str(),
                json.size() / 1e6, mp.size() / 1e6, json.size() / t1 / 1e9, out / t2 / 1e9);
  }
  return EXIT_SUCCESS;
}
