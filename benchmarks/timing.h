// Timing for the benchmarks.
#ifndef SIMDJSON_MSGPACK_TIMING_H
#define SIMDJSON_MSGPACK_TIMING_H

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
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

// Keeps the core busy for about a second, so that the first measures do not
// run at a low frequency.
inline void warm_up() {
  auto t0 = std::chrono::steady_clock::now();
  volatile uint64_t x = 1;
  while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1)) {
    for (int i = 0; i < 1000; i++) x = x * 6364136223846793005ULL + 1;
  }
}

inline std::string basename_of(const char* path) {
  std::string p(path);
  size_t slash = p.find_last_of("/\\");
  return slash == std::string::npos ? p : p.substr(slash + 1);
}

#endif  // SIMDJSON_MSGPACK_TIMING_H
