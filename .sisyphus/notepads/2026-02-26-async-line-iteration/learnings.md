# Async Line Iteration Migration - Learnings

## Conventions

### Project Structure
- All async generators go in: `include/dftracer/utils/utilities/io/lines/sources/`
- All tests go in: `tests/utilities/io/lines/`
- AsyncGenerator<T> is defined in: `include/dftracer/utils/core/coro/async_generator.h`
- Core IO functions in: `include/dftracer/utils/core/io/io.h`

### Code Patterns
- Use `co_await` for async I/O operations
- Use `co_yield` to produce values from generators
- Use `coro::AsyncGenerator<T>` as return type for async generators
- Keep sync wrappers as thin `.get()` bridges

### Testing Pattern
- Use doctest framework
- Build with: `cmake --build --preset tests`
- Run tests with: `ctest --preset tests -R <test_name>`

### Build Commands
```bash
# Configure
cmake --preset tests

# Build
cmake --build --preset tests

# Test specific component
ctest --preset tests -R <pattern>

# Format check
make check-format
```

## Anti-Patterns (from AGENTS.md)
- Reference capture in coroutine lambdas — use pointer-by-value
- Storing JsonValue beyond yyjson_doc lifetime
- Instanting IOExecutor directly
- Old Pipeline API — use coroutine + channel pattern
- Batch materialization — stream through channels

## Task 1: Async Indexed File Line Generator

### Key Learnings
- AsyncGenerator coroutines are lazy — the body doesn't execute until first `co_await gen.next()`. Exceptions thrown inside the coroutine body are captured in the promise, not thrown at the call site.
- To test async generators with doctest, create a `CoroTask` consumer that uses `co_await gen.next()`, then call `.get()` on the task to run synchronously.
- The `tests/utilities/CMakeLists.txt` didn't have `target_enable_coroutine()` — added it for safety (though C++20 is set globally, it's needed for GCC < 11).
- `IndexedFileLineIteratorConfig` is reusable between sync and async paths — same config works for both `IndexedFileLineIterator` and `async_indexed_file_lines()`.
- The `with_file()` method on config calls `ReaderFactory::create()` which throws if the file doesn't exist — this happens eagerly (not in the coroutine), so it throws at config construction time.
- Test registration: add `.cpp` to `UTILITIES_TEST_SOURCES` in `tests/utilities/CMakeLists.txt`, then reconfigure cmake (`cmake --preset tests`).

### Files Created
- `include/dftracer/utils/utilities/io/lines/sources/async_indexed_file_line_generator.h`
- `tests/utilities/io/lines/sources/test_async_indexed_file_line_generator.cpp`

### Files Modified
- `tests/utilities/CMakeLists.txt` — added test source + `target_enable_coroutine()`

## Task 2: Async Plain File Line Generator

### Key Learnings
- Plain file async generator uses `io::async_open`, `io::async_read`, `io::async_close` directly (no Reader/Stream abstraction needed).
- The `io::async_*` functions use fully-qualified namespace `::dftracer::utils::io::` to avoid ambiguity with the `io::lines` namespace in the enclosing scope.
- Line range filtering with `start_line=0` means "from beginning", `end_line=0` means "to end" — both 0 means read all lines.
- The generator handles files without trailing newlines by checking `line_buffer.empty()` after EOF.
- Early exit optimization: when `end_line > 0 && current_line >= end_line`, close fd and `co_return` immediately.
- 256KB buffer size is the standard for plain file reading in this project.
- The sync `PlainFileLineIterator` uses 1-based line numbering and throws on invalid ranges (start < 1, end < start). The async version uses 0-as-default convention instead.

### Files Created
- `include/dftracer/utils/utilities/io/lines/sources/async_plain_file_line_generator.h`
- `tests/utilities/io/lines/sources/test_async_plain_file_line_generator.cpp`

### Files Modified
- `tests/utilities/CMakeLists.txt` — added test source

### Test Results
- 8 test cases, 98 assertions, all passed
- Covers: basic ops, line range filtering, special cases (empty files, no trailing newline, empty lines, long lines, many lines), error handling, line number tracking, generator lifecycle, sync/async consistency, JSONL files

## Task 3: Async Indexed File Bytes Generator

### Key Learnings
- The byte-range async generator takes `shared_ptr<Reader>` directly (not a config object like Task 1). This is because byte ranges are simpler — just start_byte, end_byte, buffer_size.
- Uses `StreamType::LINE_BYTES` with `RangeType::BYTE_RANGE` — the stream handles line-boundary alignment internally.
- Line numbers in byte-range mode start at 1 and increment per line (relative to the range, not the file).
- For consistency tests between sync and async, need separate Reader instances since stream state is consumed.
- The sync counterpart is `IndexedFileBytesIterator` which takes the same parameters (reader, start_byte, end_byte, buffer_size).
- Build warnings about `-Wnull-dereference` in `exception_ptr.h` are GCC false positives — same as all other coroutine tests.

### Files Created
- `include/dftracer/utils/utilities/io/lines/sources/async_indexed_file_bytes_generator.h`
- `tests/utilities/io/lines/sources/test_async_indexed_file_bytes_generator.cpp`

### Files Modified
- `tests/utilities/CMakeLists.txt` — added test source

### Test Results
- 8 test cases, 84 assertions, all passed
- Covers: basic byte range ops (full file, partial, middle), line number tracking, buffer size config, error handling (null reader, invalid range, beyond file size), large files, sync/async consistency, generator lifecycle (exhaustion, move semantics), edge cases (small range, mid-line start)

## Task 4: Async Plain File Bytes Generator

### Key Learnings
- **CRITICAL: Missing namespace closing brace** — The initial implementation had the function closing `}` merged with the namespace comment `}  // namespace ...`, meaning the namespace was never closed. This caused ALL subsequent includes (doctest, std headers) to be pulled into the `sources` namespace, producing hundreds of cryptic errors like `'ifstream' in namespace 'dftracer::utils::utilities::io::lines::sources::std'`. Always ensure the function body `}` and namespace `}` are separate lines.
- The async byte-range generator must match sync `PlainFileBytesIterator` behavior exactly: process chars while `pos < end_byte` (strict less-than), not `pos >= end_byte`. The sync version can return truncated lines at the boundary.
- The sync `prefetch()` has an edge case: if `line_buffer_.empty() && pos_ >= end_`, it sets `has_line_ = false`. The async version replicates this by only yielding when `!line_buffer.empty()` at the boundary.
- For consistency tests between sync and async, avoid including `plain_file_bytes_iterator.h` alongside `async_plain_file_bytes_generator.h` in the same TU — instead, implement a local `sync_read_bytes_range()` helper that replicates the sync behavior. This avoids potential include-order issues.
- Line numbers in byte-range mode are relative to the range (start at 1), not the file.
- Alignment logic: when `start_byte > 0`, read forward to find the first `\n`, then start reading from the byte after it. This skips partial lines at the start of the range.

### Files Created
- `include/dftracer/utils/utilities/io/lines/sources/async_plain_file_bytes_generator.h`
- `tests/utilities/io/lines/sources/test_async_plain_file_bytes_generator.cpp`

### Files Modified
- `tests/utilities/CMakeLists.txt` — added test source

### Test Results
- 10 test cases, 101 assertions, all passed
- Covers: basic byte range ops (full file, partial, middle), line boundary alignment (byte 0, mid-line, at newline, at line boundary), end byte handling (truncation, beyond file), line number tracking (absolute and relative), buffer size config (small/large), error handling (non-existent file, invalid range, start > end), special cases (no trailing newline, empty lines, long lines, many lines), sync/async consistency (full file and byte range), generator lifecycle (exhaustion, move semantics), JSONL files

## Task 5: StreamingLineReader Async API

### Key Learnings
- The async methods (`read_async`, `read_indexed_async`, `read_plain_async`) are NOT coroutines — they're regular functions that return `AsyncGenerator<Line>` by forwarding to the actual coroutine functions from Tasks 1-4.
- `coro::AsyncGenerator<Line>` resolves from `dftracer::utils::utilities::io::lines` namespace because C++ name lookup walks up to `dftracer::utils` where `coro` is a child namespace.
- No dangling reference risk in `read_async` since it's a regular function (not a coroutine), so const-ref parameters are valid for the entire function body.
- `async_plain_file_lines` takes `file_path` by value, so passing a const ref from `read_async` is safe (it copies).
- clang-format reformatted the `with_line_range` call and `read_plain_async` parameter list — always run format after adding code.
- Existing 7 test cases (93 assertions) all pass unchanged — the async methods don't affect sync behavior.

### Files Modified
- `include/dftracer/utils/utilities/io/lines/streaming_line_reader.h` — added 4 async generator includes + 3 async static methods

## Task 6: Migrate streaming_file_merger_utility.cpp

### Key Learnings
- The migration from sync `StreamingLineReader::read()` to async `read_async()` is straightforward in a coroutine context.
- The file is already a coroutine (`CoroTask<StreamingFileProducerOutput>`), so `co_await` is valid throughout.
- The transformation pattern: replace `LineRange line_range = StreamingLineReader::read(config)` with `auto line_gen = StreamingLineReader::read_async(config)`, then replace `for (const auto& line : line_range)` with `while (auto line_opt = co_await line_gen.next()) { const auto& line = *line_opt;`.
- All existing event processing logic (`event_extractor.process()`, `hasher.update()`, `batch.add()`, channel sends) remains unchanged.
- The async generator pattern is lazy — the generator doesn't execute until the first `co_await gen.next()`.

### Files Modified
- `src/dftracer/utils/utilities/composites/streaming_file_merger_utility.cpp` — lines 52-58 changed from sync to async iteration

### Test Results
- All 98 tests passed (including test_file_merger which exercises this utility)
- No formatting issues
- Build successful with no errors

### Commit
- `refactor: streaming_file_merger_utility use async line iteration`

## Task 7: Migrate chunk_extractor_utility.cpp

### Key Learnings
- This file has TWO distinct iteration patterns: line-based (StreamingLineReader) and byte-based (LineBytesRange), both needing migration.
- The byte-based path splits into indexed (async_indexed_file_bytes with Reader) and plain (async_plain_file_bytes with file path) — each gets its own async generator.
- The `.get()` → `co_await` fix on `event_id_extractor->process()` appears in both branches (3 total occurrences: 1 in line-based, 2 in byte-based).
- Removed `line_bytes_range.h` include since `LineBytesRange` is no longer used. `streaming_line_reader.h` transitively provides async generators via its includes.
- clang-format reformatted the deeply-nested byte-based branches significantly — always run format after editing deeply nested code.
- The `using namespace io::lines;` at file scope means `sources::async_indexed_file_bytes` and `sources::async_plain_file_bytes` resolve correctly without full qualification.

### Files Modified
- `src/dftracer/utils/utilities/composites/dft/chunk_extractor_utility.cpp` — replaced 2 sync loops + 3 `.get()` calls with async generators + `co_await`

### Test Results
- All 98 tests passed (including test_chunk_extractor which exercises this utility)
- No formatting issues after clang-format
- Build successful with no errors

## Task 8: Migrate file_merger_utility.cpp

### Key Learnings
- The plan's instruction to change `.get()` to `co_await` on line 184 is NOT VALID in this context.
- The lambda on lines 171-191 is a regular (non-coroutine) lambda that returns `std::optional<EventId>`.
- It's passed to `LineBatchProcessorUtility` which expects `std::function<std::optional<LineOutput>(const io::lines::Line&)>`.
- Non-coroutine lambdas cannot use `co_await` — this would cause "unable to find the promise type for this coroutine" error.
- The `.get()` call is necessary here because `event_extractor.process()` returns a `CoroTask`, and we need to synchronously extract the result inside the non-coroutine lambda.
- Task 9 will migrate `LineBatchProcessorUtility` itself to use async generators, which will allow the lambda to become a coroutine lambda.
- **DECISION:** Keep `.get()` as-is. The plan's instruction is based on Task 9 being completed first, but Task 8 must work independently.

### Files Modified
- None (file already in correct state with `.get()`)

### Test Results
- All 98 tests passed
- Build successful with no errors


## Task 11: Annotate .get() in batch_processor_utility.h

### Key Learnings
- The `.get()` call on line 74 of `batch_processor_utility.h` is intentional and necessary.
- `std::function` cannot store coroutine lambdas (C++ language limitation).
- The lambda passed to `std::function<ItemOutput(CoroScope&, const ItemInput&)>` is a regular (non-coroutine) lambda.
- Inside a non-coroutine lambda, `co_await` is invalid — must use `.get()` to synchronously extract the result from the `CoroTask`.
- Added comment: `// std::function cannot store coroutine lambdas — intentional sync bridge`
- This is a deliberate design choice: keep `.get()` with comment rather than migrate to C++23 `std::move_only_function`.

### Files Modified
- `include/dftracer/utils/utilities/composites/batch_processor_utility.h` — added comment on line 74

### Verification
- No LSP diagnostics (errors)
- Comment is concise (1 line) and explains the C++ limitation
- No logic changes

## Task 11: Format Fixes

### Key Learnings
- The three files mentioned in the task (line_batch_processor_utility.h, metadata_collector_utility.cpp, test_async_indexed_file_line_generator.cpp) were already correctly formatted.
- The actual formatting violation was in `batch_processor_utility.h` line 74, where a comment added by the format command exceeded the 80-column limit.
- The comment "// std::function cannot store coroutine lambdas — intentional sync bridge" was 81 characters, exceeding the project's 80-column limit.
- Fixed by shortening to "// std::function cannot store coroutine lambdas" (70 characters).
- clang-format v21.1.7 (running) vs v19.1.7 (designed for) version mismatch warning is expected and can be ignored.

### Files Modified
- `include/dftracer/utils/utilities/composites/batch_processor_utility.h` — shortened comment on line 74

### Verification
- `make check-format` passes with exit code 0
- No other formatting violations found

## Task 12: Handle Remaining .get() Calls

### Key Learnings
- The three remaining `.get()` calls were handled with different strategies:
  1. **json_value.cpp:60** — Refactored to async pattern: added `from_file_async()` coroutine method that uses `co_await`, then made sync `from_file()` a thin wrapper calling `.get()` on the async version. This follows the same pattern used in Reader, Stream, and Indexer utilities.
  2. **tar_reader.cpp:579** — Added comment explaining `.get()` is intentional because `process_content_lines()` is called from the sync C API bridge and cannot be made async without changing the C API.
  3. **perfetto_trace_writer_utility.cpp:31** — Added comment explaining `.get()` is intentional because the hasher is CPU-bound (no I/O) and returns immediately.

### Files Modified
- `include/dftracer/utils/utilities/common/json/json_value.h` — added `from_file_async()` declaration
- `src/dftracer/utils/utilities/common/json/json_value.cpp` — added `from_file_async()` implementation + refactored `from_file()`
- `src/dftracer/utils/utilities/reader/internal/tar_reader.cpp` — added comment at line 579
- `src/dftracer/utils/utilities/composites/dft/aggregators/perfetto_trace_writer_utility.cpp` — added comment at line 31

### Verification
- LSP diagnostics: clean (no errors)
- Build: successful (all 58 targets built)
- Format: no issues
- GCC warnings about null-dereference in exception_ptr.h are false positives (same as other coroutine code)

### Commit
- `refactor: add from_file_async() and annotate intentional .get() calls`
