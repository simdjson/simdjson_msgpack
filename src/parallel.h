// Internal: running work on several threads.
#ifndef SIMDJSON_MSGPACK_PARALLEL_H
#define SIMDJSON_MSGPACK_PARALLEL_H

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <shared_mutex>
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

// Calls fn(worker) on up to `threads` threads (the calling thread is worker
// 0), and joins them. fn must not throw. If a thread cannot be started, the
// threads we have do the work.
template <typename Fn>
void run_threads(size_t threads, Fn fn) {
  std::vector<std::thread> pool;
  try {
    pool.reserve(threads - 1);
    for (size_t t = 1; t < threads; t++) pool.emplace_back(fn, t);
  } catch (...) {  // std::system_error or std::bad_alloc: fewer threads
  }
  fn(size_t(0));
  for (std::thread& t : pool) t.join();
}

// Puts the outputs of blocks, converted on several threads, in order at the
// end of a buffer, writing each once in place. A block's offset is known once
// the sizes of the blocks before it are (they are published as the blocks are
// converted, in any order); the thread that converted the block then copies
// it (see ordered_slots).
class ordered_output {
 public:
  ordered_output(buffer& out, size_t max_blocks)
      : out_(out),
        base_(out.size()),
        max_blocks_(max_blocks),
        sizes_(new std::atomic<size_t>[max_blocks]),
        offsets_(new size_t[max_blocks + 1]) {
    for (size_t i = 0; i < max_blocks; i++) sizes_[i].store(unknown, std::memory_order_relaxed);
    offsets_[0] = 0;
  }

  // Block i was converted into n bytes.
  void converted(size_t i, size_t n) {
    sizes_[i].store(n, std::memory_order_release);
    std::lock_guard<std::mutex> lock(prefix_);
    size_t k = known_.load(std::memory_order_relaxed);
    while (k < max_blocks_) {
      const size_t size = sizes_[k].load(std::memory_order_acquire);
      if (size == unknown) break;
      offsets_[k + 1] = offsets_[k] + size;
      k++;
    }
    known_.store(k, std::memory_order_release);
  }
  // Whether the offset of block i is known.
  bool placed(size_t i) const { return i < known_.load(std::memory_order_acquire); }
  // Copies block i (whose offset is known): n bytes at p. Throws
  // std::bad_alloc when the buffer cannot grow, then and ever after: an
  // allocator may have freed the buffer, which we must no longer write.
  void copy(size_t i, const char* p, size_t n) {
    const size_t at = offsets_[i];
    if (n == 0) return;
    bool fits;
    {
      std::shared_lock<std::shared_mutex> lock(mutex_);
      fits = !lost_ && out_.capacity() - base_ >= at + n;
    }
    if (!fits) {
      // No copy is under way (they hold the shared lock): the buffer keeps
      // what has been copied, the first base_ + high_ bytes.
      std::unique_lock<std::shared_mutex> lock(mutex_);
      if (!lost_ && out_.capacity() - base_ < at + n) {
        const size_t high = high_.load(std::memory_order_relaxed);
        out_.resize(base_ + high);
        try {
          out_.reserve(at + n - std::min(high, at + n));
        } catch (...) {
          lost_ = true;
        }
      }
    }
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (lost_) throw std::bad_alloc();
    std::memcpy(out_.data() + base_ + at, p, n);
    size_t high = high_.load(std::memory_order_relaxed);
    while (high < at + n && !high_.compare_exchange_weak(high, at + n)) {
    }
  }
  // Whether the buffer could not grow (once the threads are done).
  bool lost() const { return lost_; }
  // Sets the size of the buffer, once the blocks [0, blocks) are copied.
  void finish(size_t blocks) { out_.resize(base_ + offsets_[blocks]); }

 private:
  static constexpr size_t unknown = ~size_t(0);
  buffer& out_;
  const size_t base_;
  const size_t max_blocks_;
  std::unique_ptr<std::atomic<size_t>[]> sizes_;
  std::unique_ptr<size_t[]> offsets_;  // offsets_[0, known_] are set (under prefix_)
  std::atomic<size_t> known_{0};       // the blocks whose offsets are known
  std::mutex prefix_;
  std::shared_mutex mutex_;  // copies (shared) against growth (exclusive)
  bool lost_{false};         // the buffer could not grow (under mutex_)
  std::atomic<size_t> high_{0};  // the end of what has been copied
};

// The buffers in which one thread converts blocks, for an ordered_output: it
// goes on to the next block while the ones it converted wait for their
// offsets, and copies them as they become known. A thread waits only when all
// its buffers are pending: it takes a buffer before it takes a block, so that
// the blocks of lower index, which its pending blocks wait for, are all taken
// by threads that are converting them (no deadlock).
class ordered_slots {
 public:
  ordered_slots(ordered_output& out, std::vector<std::unique_ptr<buffer>>& buffers)
      : out_(out) {
    for (auto& b : buffers) free_.push_back(b.get());
  }
  // An empty buffer to convert a block into, or null if stop became true.
  buffer* take(const std::atomic<bool>& stop) {
    copy_ready();
    while (free_.empty()) {
      if (stop.load(std::memory_order_relaxed)) return nullptr;
      std::this_thread::yield();
      copy_ready();
    }
    buffer* b = free_.back();
    free_.pop_back();
    b->resize(0);
    return b;
  }
  // b holds block i.
  void converted(size_t i, buffer* b) {
    out_.converted(i, b->size());
    pending_.push_back(slot{i, b});
    copy_ready();
  }
  // Copies all the pending blocks; returns false if stop became true.
  bool flush(const std::atomic<bool>& stop) {
    copy_ready();
    while (!pending_.empty()) {
      if (stop.load(std::memory_order_relaxed)) return false;
      std::this_thread::yield();
      copy_ready();
    }
    return true;
  }

 private:
  struct slot {
    size_t block;
    buffer* data;
  };
  // Pending blocks are in increasing order: when one is not placed, nor are
  // the ones after it.
  void copy_ready() {
    while (!pending_.empty() && out_.placed(pending_.front().block)) {
      slot s = pending_.front();
      out_.copy(s.block, s.data->data(), s.data->size());
      pending_.erase(pending_.begin());
      free_.push_back(s.data);
    }
  }
  ordered_output& out_;
  std::vector<buffer*> free_;
  std::vector<slot> pending_;
};

// How many buffers each thread has for an ordered_output.
constexpr size_t slots_per_thread = 4;

}  // namespace internal
}  // namespace simdjson_msgpack

#endif  // SIMDJSON_MSGPACK_PARALLEL_H
