// Timing for the benchmarks.
#ifndef SIMDJSON_MSGPACK_TIMING_H
#define SIMDJSON_MSGPACK_TIMING_H

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

// Runs fn (which returns false on failure) repeatedly, in rounds of at least
// 50 ms, and returns the best time of one call, in seconds (or a negative
// value if fn failed).
template <typename F>
double best_time(int rounds, F fn) {
  using clock = std::chrono::steady_clock;
  size_t calls = 1;
  // Calibrate: enough calls for 50 ms.
  for (;;) {
    auto t0 = clock::now();
    for (size_t i = 0; i < calls; i++) {
      if (!fn()) return -1;
    }
    double t = std::chrono::duration<double>(clock::now() - t0).count();
    if (t >= 0.05 || calls > (size_t(1) << 30)) break;
    calls *= 2;
  }
  double best = 1e300;
  for (int r = 0; r < rounds; r++) {
    auto t0 = clock::now();
    for (size_t i = 0; i < calls; i++) {
      if (!fn()) return -1;
    }
    best = std::min(best, std::chrono::duration<double>(clock::now() - t0).count() / calls);
  }
  return best;
}

inline std::string basename_of(const char* path) {
  std::string p(path);
  size_t slash = p.find_last_of("/\\");
  return slash == std::string::npos ? p : p.substr(slash + 1);
}

#endif  // SIMDJSON_MSGPACK_TIMING_H
