// Internal: the buffer in which the writers put their output.
#ifndef SIMDJSON_MSGPACK_BUFFER_H
#define SIMDJSON_MSGPACK_BUFFER_H

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>

#include "simdjson_msgpack.h"

namespace simdjson_msgpack {
namespace internal {

// A buffer from malloc (or from an allocator), which we can hand over without
// a copy. It grows without being zeroed (unlike a std::string).
class buffer {
 public:
  buffer() = default;
  // We copy the allocator: it need not outlive the buffer.
  explicit buffer(const allocator* a) {
    if (a != nullptr) {
      allocator_ = *a;
      custom_ = true;
    }
  }
  buffer(const buffer&) = delete;
  buffer& operator=(const buffer&) = delete;
  ~buffer() { free(data_); }

  size_t size() const { return size_; }
  size_t capacity() const { return capacity_; }
  char* data() { return data_; }
  std::string_view view() const { return std::string_view(data_, size_); }

  // Room for n more bytes, at the end; throws std::bad_alloc if there is none.
  char* reserve(size_t n) {
    if (simdjson_unlikely(capacity_ - size_ < n)) grow(n);
    return data_ + size_;
  }
  void append(const void* p, size_t n) {
    std::memcpy(reserve(n), p, n);
    size_ += n;
  }
  // Sets the size (to at most the size and the room reserved; at most 0 once
  // the buffer is lost).
  void resize(size_t size) { size_ = size < capacity_ ? size : capacity_; }
  // Gives up the buffer, followed by a zero byte. We shrink it when much of
  // it is unused, before we write the zero: an allocator keeps only the used
  // bytes.
  // Null (and *size 0) if the allocator cannot shrink it: it is lost.
  char* release(size_t* size) {
    reserve(1);
    if (capacity_ - size_ > 4096 && capacity_ - size_ > size_ / 4) {
      char* p = reallocate(data_, size_ + 1);
      if (p != nullptr) {
        data_ = p;
        capacity_ = size_ + 1;
      } else if (custom_) {
        lose();
        *size = 0;
        return nullptr;
      }
    }
    data_[size_] = '\0';
    char* p = data_;
    *size = size_;
    data_ = nullptr;
    size_ = capacity_ = 0;
    return p;
  }

 private:
  char* reallocate(char* p, size_t capacity) {
    return custom_ ? allocator_.reallocate(allocator_.context, p, size_, capacity)
                   : static_cast<char*>(std::realloc(p, capacity));
  }
  void free(char* p) {
    if (!custom_) {
      std::free(p);
    } else if (p != nullptr) {
      allocator_.deallocate(allocator_.context, p);
    }
  }
  void grow(size_t n) {
    size_t capacity = capacity_ < 4096 ? 4096 : capacity_ * 2;
    if (capacity - size_ < n) capacity = size_ + n;
    char* p = reallocate(data_, capacity);
    if (p == nullptr) {
      lose();
      throw std::bad_alloc();
    }
    data_ = p;
    capacity_ = capacity;
  }
  // The allocator could not reallocate data_: an allocator now owns it (and
  // may have freed it); realloc left it to us.
  void lose() {
    if (!custom_) std::free(data_);
    data_ = nullptr;
    size_ = capacity_ = 0;
  }

  allocator allocator_{};
  bool custom_{false};  // allocator_ is the caller's (otherwise: malloc)
  char* data_{nullptr};
  size_t size_{0};
  size_t capacity_{0};
};

}  // namespace internal
}  // namespace simdjson_msgpack

#endif  // SIMDJSON_MSGPACK_BUFFER_H
