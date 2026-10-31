// Internal: running work on several threads.
#ifndef SIMDJSON_MSGPACK_PARALLEL_H
#define SIMDJSON_MSGPACK_PARALLEL_H

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstddef>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include "buffer.h"

namespace simdjson_msgpack {
namespace internal {

// Calls fn(i, worker) for i in [0, n), with up to `threads` threads (the
// calling thread is one of them); worker in [0, threads) tells the threads
// apart, so that each may keep its own state. Each thread takes the next i from a shared counter.
// If fn throws, the threads stop taking work, we join them all and rethrow
// the first exception on the calling thread. If a thread cannot be started,
// the threads we have do the work.
template <typename Fn>
void parallel_for(size_t n, size_t threads, Fn fn) {
  threads = std::min(threads, n);
  if (threads <= 1) {
    for (size_t i = 0; i < n; i++) fn(i, size_t(0));
    return;
  }
  std::atomic<size_t> next{0};
  std::mutex mutex;
  std::exception_ptr error;
  auto work = [&](size_t worker) {
    try {
      for (size_t i = next++; i < n; i = next++) fn(i, worker);
    } catch (...) {
      next = n;  // the others stop
      std::lock_guard<std::mutex> lock(mutex);
      if (!error) error = std::current_exception();
    }
  };
  std::vector<std::thread> pool;
  try {
    pool.reserve(threads - 1);
    for (size_t t = 1; t < threads; t++) pool.emplace_back(work, t);
  } catch (...) {  // std::system_error or std::bad_alloc: fewer threads
  }
  work(size_t(0));
  for (std::thread& t : pool) t.join();
  if (error) std::rethrow_exception(error);
}

// Where a block, converted by one of several threads, sits: in the buffer of
// the thread that converted it.
struct block_output {
  size_t worker{0};
  size_t offset{0};
  size_t size{0};
};

// Appends the blocks, in order, to out (copying on up to `threads` threads).
inline void gather(buffer& out, const std::vector<buffer*>& workers,
                   const std::vector<block_output>& blocks, size_t threads) {
  std::vector<size_t> at(blocks.size() + 1, out.size());
  for (size_t i = 0; i < blocks.size(); i++) at[i + 1] = at[i] + blocks[i].size;
  out.reserve(at.back() - out.size());
  char* data = out.data();
  parallel_for(blocks.size(), threads, [&](size_t i, size_t) {
    const block_output& b = blocks[i];
    if (b.size != 0) std::memcpy(data + at[i], workers[b.worker]->data() + b.offset, b.size);
  });
  out.resize(at.back());
}

}  // namespace internal
}  // namespace simdjson_msgpack

#endif  // SIMDJSON_MSGPACK_PARALLEL_H
