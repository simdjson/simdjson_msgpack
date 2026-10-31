# simdjson_msgpack

JSON <=> MessagePack, using simdjson.

simdjson_msgpack converts JSON to [MessagePack](https://msgpack.org/), with
[simdjson](https://github.com/simdjson/simdjson), and MessagePack back to JSON.
It writes the most compact MessagePack, and it is faster than the usual ways
of doing it in C and C++ (yyjson, RapidJSON with mpack, nlohmann::json): see
[Performance](#performance).

- One document, or many: newline-delimited JSON (NDJSON, JSON Lines) becomes
  a sequence of MessagePack values, and back.
- On many threads, for newline-delimited JSON and sequences of values.
- The smallest encodings: integers take the smallest integer encoding,
  strings, arrays and maps the smallest header. The output is byte for byte
  what msgpack-python (`msgpack.packb(json.loads(text))`) gives.
- Output in a buffer of your own, if you want: say, a buffer of another
  language, handed over without a copy.
- No dependency but simdjson (and
  [zmij](https://github.com/vitaut/zmij), included, to write floating-point
  numbers).
- C++17, one header and one source file. A C interface too.

Status: early (0.1). The interface may change.

## Contents

- [Adding it to your project](#adding-it-to-your-project)
- [Quick start](#quick-start)
- [JSON to MessagePack](#json-to-messagepack)
- [MessagePack to JSON](#messagepack-to-json)
- [Many documents, many threads](#many-documents-many-threads)
- [Writing into your own buffer](#writing-into-your-own-buffer)
- [The C interface](#the-c-interface)
- [Errors, memory and threads](#errors-memory-and-threads)
- [Building and testing](#building-and-testing)
- [Performance](#performance)
- [License](#license)

## Adding it to your project

The simplest way: take two files from a
[release](https://github.com/simdjson/simdjson_msgpack/releases),
`simdjson_msgpack.h` and `simdjson_msgpack.cpp` (or `singleheader.zip`, which
holds both), and the two files of a
[simdjson release](https://github.com/simdjson/simdjson/releases) (5.0.7 or
later), `simdjson.h` and `simdjson.cpp`. Put the four files in one directory,
include `simdjson_msgpack.h`, and compile the two source files with yours, as
C++17:

```sh
c++ -O3 -std=c++17 myprogram.cpp simdjson_msgpack.cpp simdjson.cpp -o myprogram
```

With MSVC: `cl /std:c++17 /O2 /EHsc myprogram.cpp simdjson_msgpack.cpp simdjson.cpp`.
Add `-pthread` if your toolchain needs it for `std::thread`. If you already
use simdjson, keep your copy: `simdjson_msgpack.h` includes `"simdjson.h"`,
which must be on the include path. If you already compile zmij (as
[simdjson_arrow](https://github.com/simdjson/simdjson_arrow) users may),
define `SIMDJSON_MSGPACK_EXTERNAL_ZMIJ`: `simdjson_msgpack.cpp` then includes
your `zmij.h` rather than its own copy.

With CMake, the `simdjson_msgpack` target brings simdjson with it:

```cmake
include(FetchContent)
FetchContent_Declare(simdjson_msgpack
  GIT_REPOSITORY https://github.com/simdjson/simdjson_msgpack.git
  GIT_TAG main)
set(SIMDJSON_MSGPACK_TESTS OFF CACHE BOOL "" FORCE)
set(SIMDJSON_MSGPACK_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SIMDJSON_MSGPACK_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(simdjson_msgpack)
target_link_libraries(myprogram PRIVATE simdjson_msgpack)
```

From a checkout, `python3 tools/amalgamate.py` writes the two single files in
`singleheader/`.

## Quick start

```cpp
#include "simdjson_msgpack.h"
using namespace simdjson_msgpack;

simdjson::padded_string json = simdjson::padded_string::load("twitter.json");

std::string msgpack;
status s = json_to_msgpack(json, &msgpack);
if (!s.ok()) { /* s.message: "TAPE_ERROR: ..." */ }

std::string back;
s = msgpack_to_json(msgpack.data(), msgpack.size(), &back);
```

[`examples/example.cpp`](examples/example.cpp) is a complete program.

## JSON to MessagePack

```cpp
class msgpack_writer {
 public:
  explicit msgpack_writer(const msgpack_options& options = {});
  msgpack_writer(const msgpack_options& options, const allocator& allocator);
  status write_json(simdjson::padded_string_view json);  // one document, one value
  status write_json(const char* json, size_t length);    // the same, without padding (copied)
  status write_ndjson(simdjson::padded_string_view json, size_t* count = nullptr);
  status write_ndjson(const char* json, size_t length, size_t* count = nullptr);
  std::string_view view() const;    // the MessagePack so far
  char* release(size_t* size);      // gives up the buffer (free it with free)
  void clear();                     // forgets the output, keeps the buffer
};
status json_to_msgpack(simdjson::padded_string_view json, std::string* out,
                       const msgpack_options& options = {});
status ndjson_to_msgpack(simdjson::padded_string_view json, std::string* out,
                         const msgpack_options& options = {});
```

Successive calls append to the output; a call that fails writes nothing. A
writer keeps its simdjson parser and its buffers: keep one (per thread) to
convert many small documents.

| JSON | MessagePack |
|---|---|
| `null`, `true`, `false` | nil, false, true |
| integers from 0 to 2^64-1 | the smallest of positive fixint, uint 8, 16, 32, 64 |
| integers from -2^63 to -1 | the smallest of negative fixint, int 8, 16, 32, 64 |
| other numbers | float 64 (or float 32 when exact, with `float32_when_exact`) |
| integers beyond 64 bits | float 64 (or an error, without `big_integers_as_float64`) |
| strings | fixstr, str 8, 16 or 32 (unescaped, UTF-8) |
| arrays | fixarray, array 16 or 32 |
| objects | fixmap, map 16 or 32, keys and values in order (duplicate keys too) |

```cpp
struct msgpack_options {
  bool big_integers_as_float64{true};  // integers beyond 64 bits become float64 (or an error)
  bool float32_when_exact{false};      // float32 when it holds the number exactly
  size_t threads{1};                   // for newline-delimited JSON
  size_t block_size{1 << 20};          // bytes of JSON per thread and block
};
```

The JSON is validated as simdjson does (including UTF-8). We write at most
`max_msgpack_size(json.size())` bytes (3 per byte of JSON, plus 64).

How it works: simdjson's On-Demand parser gives the elements of an array or
an object one after the other, so we do not know how many there are when we
write the header. We leave 5 bytes for each header, note where, and once the
document is written, put the smallest headers in place, moving the bytes back
(one pass over the output). Strings are unescaped right after one byte left
for a short header, and moved if they need a longer one.

## MessagePack to JSON

```cpp
class json_writer {
 public:
  explicit json_writer(const json_options& options = {});
  json_writer(const json_options& options, const allocator& allocator);
  status write_msgpack(const void* data, size_t length);  // one value: all of data
  status write_msgpack_stream(const void* data, size_t length, size_t* count = nullptr);
  std::string_view view() const;  // the JSON so far
  char* release(size_t* size);    // gives up the buffer (free it with free), zero-terminated
  void clear();
};
status msgpack_to_json(const void* data, size_t length, std::string* out,
                       const json_options& options = {});
status msgpack_to_ndjson(const void* data, size_t length, std::string* out,
                         const json_options& options = {});
```

```cpp
enum class ext_policy { error, null };
struct json_options {
  bool validate_utf8{true};     // strings must be valid UTF-8 (an error otherwise)
  bool binary_as_base64{true};  // binary values as base64 strings (an error otherwise)
  ext_policy ext{ext_policy::error};  // extension values (timestamps too): an error, or null
  size_t max_depth{1024};       // deeper nesting is an error
  size_t threads{1};            // for sequences of values
  size_t block_size{1 << 20};   // bytes of MessagePack per thread and block
};
```

- Strings are escaped as JSON requires (quotes, backslashes, control
  characters); other characters are written as they are (UTF-8).
- Floating-point numbers get the shortest representation that gives them back
  (float32 values as float32), with a decimal point or an exponent (`1.0`, not
  `1`, so that they read back as floating-point numbers). NaN and infinities,
  which JSON lacks, become `null`.
- Map keys that are not strings become strings: integers, floats, booleans and
  nil as their JSON text (`"1"`, `"true"`, `"null"`), binary values in base64.
  Array, map and extension keys are an error.
- We check every length against the input: truncated or malformed input is an
  error, never a read out of bounds. Errors give the byte where the value
  begins: `truncated string (byte 1234)`, `invalid byte 0xc1 (byte 3)`.

## Many documents, many threads

`write_ndjson` reads JSON documents separated by whitespace (usually one per
line) and writes each as one MessagePack value, one after the other: a
MessagePack stream. `write_msgpack_stream` does the reverse, writing each
value as a line of JSON.

With `threads` above one:

- JSON to MessagePack: threads convert blocks of about `block_size` bytes, cut
  at line ends (as simdjson_arrow does). A document must then fit on one line;
  if one does not, or if a block fails, we convert again on one thread, so that
  documents may span lines and errors are the same whatever the number of
  threads.
- MessagePack to JSON: MessagePack has no line ends, so the calling thread
  finds where the values end (skipping over them, which is quick) and hands
  blocks of about `block_size` bytes to the other threads as it goes, then
  writes blocks too. On error, we write again on one thread, for the same
  error.

Each thread writes into its own buffer (kept by the writer between calls);
we then copy the blocks, in order, in parallel.

## Writing into your own buffer

By default the writers grow a buffer from `malloc`. To write into memory of
your own (say, a buffer of a Python `bytes` object), give them an
`allocator`; `release` then gives up a buffer from your `reallocate`:

```cpp
struct allocator {
  // A buffer of at least size bytes beginning with the used bytes of p (null at
  // first), or null if it cannot.
  char* (*reallocate)(void* context, char* p, size_t used, size_t size);
  void (*deallocate)(void* context, char* p);  // frees a buffer not given up
  void* context;
};
```

The writers keep a copy of the allocator (the functions and the context must
stay valid, not the struct). `release` shrinks the buffer (through
`reallocate`) when much of it is unused; the buffer it gives up holds a zero
byte after its `*size` bytes, with your allocator too.

## The C interface

Three functions with C linkage, for programs or languages that cannot call
C++:

```c
// JSON to MessagePack: one document, or with ndjson non-zero, many. With
// capacity >= length + SIMDJSON_PADDING, the input is read in place.
int simdjson_msgpack_json_to_msgpack(const char* data, size_t length, size_t capacity,
                                     int ndjson, size_t threads, char** out,
                                     size_t* out_length, char* error, size_t error_capacity);
// MessagePack to JSON: one value, or with stream non-zero, many (as NDJSON).
int simdjson_msgpack_msgpack_to_json(const char* data, size_t length, int stream,
                                     int validate_utf8, int binary_as_base64, int ext_as_null,
                                     size_t threads, char** out, size_t* out_length,
                                     char* error, size_t error_capacity);
void simdjson_msgpack_free(void* p);
```

They return 0 on success, with `*out` to free with `simdjson_msgpack_free`;
otherwise a non-zero value, with the message in `error`.

## Errors, memory and threads

- Errors come back as a `status` (`ok()`, `message`), not as exceptions, even
  when memory runs out ("out of memory"). On error, the output is left as it
  was.
- JSON errors are simdjson's (`TAPE_ERROR: ...`, `INCORRECT_TYPE: ...`), with
  the document, for newline-delimited JSON: `... (document 20000)`.
- A writer must be used by one thread at a time; different writers may be
  used at once. The threads of `options.threads` are started for each call
  and joined before it returns.
- simdjson wants `SIMDJSON_PADDING` bytes (64) after the input: a
  `simdjson::padded_string` has them. Without them, we copy the input.

## Building and testing

```sh
cmake -B build
cmake --build build
ctest --test-dir build    # the tests and the example
```

CMake downloads simdjson 5.0.7. Options: `SIMDJSON_MSGPACK_TESTS`,
`SIMDJSON_MSGPACK_BENCHMARKS` and `SIMDJSON_MSGPACK_EXAMPLES` (on by default),
and `SIMDJSON_MSGPACK_COMPETITION` (off), which downloads nlohmann/json,
yyjson, RapidJSON and mpack and builds `bench_competition`.

The tests check the encodings byte by byte (every header size, every integer
size), the errors, the threads (the same output and the same errors on one
thread or many), and random documents: round trips, and random changes to
valid JSON (which we must accept exactly when simdjson's DOM parser does) and
to valid MessagePack (an error, or valid JSON, never a crash). CI runs them
with GCC, clang (with the address, undefined-behavior and thread sanitizers),
Apple clang and MSVC. During development, we also checked our output against
msgpack-python, byte for byte, on random documents and on simdjson's sample
files.

## Performance

Intel Xeon Gold 6338 (Ice Lake, 2 GHz, 3.15 GHz under load), Linux, one core,
best of ten rounds, in GB/s of JSON: read, when writing MessagePack; written
(what we write), when writing JSON. Files from
[simdjson-data](https://github.com/simdjson/simdjson-data). Each cell: GCC 11
/ clang 21.

JSON to MessagePack:

| file | simdjson_msgpack | simdjson DOM | yyjson | RapidJSON + mpack | nlohmann::json |
|---|---|---|---|---|---|
| twitter | **2.30** / **2.10** | 1.86 / 1.87 | 1.40 / 1.39 | 0.49 / 0.44 | 0.10 / 0.10 |
| citm_catalog | **1.97** / **1.92** | 1.88 / 1.91 | 1.38 / 1.37 | 0.75 / 0.74 | 0.15 / 0.14 |
| canada | **0.69** / **0.64** | 0.45 / 0.44 | 0.41 / 0.41 | 0.22 / 0.19 | 0.06 / 0.06 |
| gsoc-2018 | **3.16** / **3.03** | 2.94 / 2.87 | 1.67 / 1.62 | 0.48 / 0.41 | 0.16 / 0.20 |
| github_events | **2.44** / **2.19** | 2.13 / 2.10 | 1.50 / 1.50 | 0.47 / 0.40 | 0.10 / 0.11 |
| mesh | **0.71** / **0.62** | 0.40 / 0.38 | 0.35 / 0.34 | 0.29 / 0.30 | 0.07 / 0.07 |
| marine_ik | **0.70** / **0.65** | 0.46 / 0.45 | 0.40 / 0.41 | 0.34 / 0.34 | 0.07 / 0.07 |

MessagePack to JSON:

| file | simdjson_msgpack | mpack + yyjson | mpack + RapidJSON | nlohmann::json |
|---|---|---|---|---|
| twitter | **1.44** / **1.66** | 0.70 / 0.71 | 0.33 / 0.39 | 0.07 / 0.06 |
| citm_catalog | **1.02** / **1.13** | 0.41 / 0.41 | 0.29 / 0.34 | 0.05 / 0.05 |
| canada | **0.66** / **0.63** | 0.39 / 0.40 | 0.20 / 0.20 | 0.09 / 0.08 |
| gsoc-2018 | **2.29** / **2.61** | 1.25 / 1.29 | 0.42 / 0.46 | 0.09 / 0.09 |
| github_events | **1.66** / **1.97** | 0.85 / 0.86 | 0.35 / 0.41 | 0.07 / 0.07 |
| mesh | **0.57** / **0.59** | 0.35 / 0.37 | 0.23 / 0.23 | 0.10 / 0.09 |
| marine_ik | **0.43** / **0.44** | 0.24 / 0.24 | 0.18 / 0.18 | 0.05 / 0.05 |

What we compare:

- simdjson DOM, yyjson: the parser of the library, then a walk of its
  document that writes the same bytes as ours.
- RapidJSON + mpack: RapidJSON's DOM, written with mpack (as msgpack-tools'
  json2msgpack does). Same bytes as ours.
- nlohmann::json: `json::parse` then `json::to_msgpack`; `json::from_msgpack`
  then `dump`. It writes some floats as float32, when exact.
- mpack + RapidJSON: mpack's node tree, written with RapidJSON's `Writer`
  (as msgpack-tools' msgpack2json does). mpack + yyjson: mpack's node tree,
  to a yyjson mutable document, written with `yyjson_mut_write`.
- `bench_competition` also runs the On-Demand code of simdjson's own
  json2msgpack benchmark (`benchmark/json2msgpack`), which does less: every
  number a float64, every string a str 32, every header 5 bytes. Its output
  is 5% to 81% larger than ours, and we are faster on every file (on twitter, it
  goes at 1.51 / 1.20 GB/s).

On an Apple M4 Max (Apple clang 17, on AC power), with the first version of
the library (96bc30b), JSON to MessagePack went at 1.2 GB/s (marine_ik) to
4.5 GB/s (gsoc-2018), ahead of yyjson on six files of seven (citm_catalog:
3.29 against 3.37 GB/s, before the changes for GCC), and MessagePack to JSON
at 0.95 to 6.6 GB/s, 2.4 to 4.5 times as fast as mpack + yyjson.

Newline-delimited JSON on several threads: one hour of
[GH Archive](https://www.gharchive.org/) (886 MB,
`curl -sSfL https://data.gharchive.org/2024-03-13-12.json.gz | gunzip > gharchive.ndjson`),
same machine (two sockets of 32 cores), GCC 11, one thread per core of one
socket up to 32 threads, then both sockets, in GB/s of JSON:

| threads | JSON to MessagePack | MessagePack to JSON |
|---|---|---|
| 1 | 1.4 | 1.2 |
| 2 | 2.3 | 1.7 |
| 4 | 4.4 | 3.4 |
| 8 | 7.8 | 4.5 |
| 16 | 12.2 | 4.7 |
| 32 | 16.7 | 4.0 |
| 64 | 18.9 | 3.3 |

Writing JSON from MessagePack stops scaling at about eight threads: one
thread must find where the values end (MessagePack has no line ends) before
the others can write them.

The machine reaches its turbo frequency only after a few seconds of load: we
run a throwaway benchmark first. With GCC, we force the inlining of the
array and object loops, and avoid copying simdjson results: GCC otherwise
copies On-Demand iterators through the stack with stores that loads cannot
forward from, which made it up to 2.4 times slower.

To run them:

```sh
./build/bench file.json...                          # this library
./build/bench --ndjson --threads 8 gharchive.ndjson
cmake -B build -DSIMDJSON_MSGPACK_COMPETITION=ON && cmake --build build
./build/bench_competition file.json...              # against the others
```

## License

Apache License 2.0. zmij (`third_party/zmij`) is under the MIT license.
