# C++ Style Guide

This project uses an STL-flavored naming convention (matching `std::atomic`,
`std::mutex::try_lock`, etc. — common in systems/low-latency C++, not tied to
any single named external style guide), **C++ Core Guidelines** for modern
C++ idioms, and a set of hot-path rules common in low-latency trading
systems. Where these sources conflict, the hot-path rules win inside
anything on the queue's push/pop path.

Note: no named public style guide is verified as "the" standard across
quant/HFT firms — those firms keep internal conventions private. These are
this project's own conventions, chosen to match common systems-C++ practice.

## Language & tooling

- Standard: C++17 (see `CMakeLists.txt`).
- Warnings: build with `-Wall -Wextra -Wpedantic`; treat new warnings as bugs.
- Prefer `clang-format` (LLVM base style with a 100 column limit and
  4-space indent, to match this file's brace/indent conventions below) and
  `clang-tidy` for static checks once the project has enough code to justify
  configuring them.
- Before merging non-trivial changes to the queue internals, build and run
  with AddressSanitizer/UndefinedBehaviorSanitizer at least once, and
  ThreadSanitizer whenever the change touches synchronization. Lock-free code
  hides races that only show up under sanitizers or heavy contention.

## Naming & formatting

- Types (`class`, `struct`, `enum`, type aliases): `PascalCase`
  (`MPMCQueue`, `Cell`).
- Functions and local/parameter variables: `snake_case` (`try_push`,
  `try_pop`).
- Private/protected member variables: `snake_case_` with a trailing
  underscore (`head_`, `tail_`, `buffer_`).
- Constants and `constexpr` values: `kPascalCase` (`kCacheLineSize`).
- Template parameters: `PascalCase` (`T`, `Capacity` are fine as-is since
  they're single concepts, but multi-word template params should still be
  `PascalCase`).
- Namespaces: lowercase, short (`lfq`, `detail` for implementation-only code
  not part of the public API).
- One statement per line; braces on the same line as the declaration (K&R /
  Google brace style); 4-space indentation, no tabs.
- File extensions: `.h` for headers, `.cc` for implementation files (e.g.
  `include/lfq/mpmc_queue.h`, `src/main.cc`) — this is the Google C++ Style
  Guide's file-naming convention specifically, adopted here independent of
  the naming/formatting choices above.

## Modern C++ idioms (Core Guidelines)

- RAII everywhere: no manual `new`/`delete` in application code; if a raw
  resource handle is unavoidable, wrap it in a small RAII type.
- Prefer `const` by default; make a variable, parameter, or member mutable
  only when it needs to change.
- Pass small trivial types by value, everything else by `const&` unless the
  function needs to take ownership (then take by value and `std::move`).
- Prefer `enum class` over unscoped `enum`.
- Use `= delete` / `= default` explicitly for special member functions
  instead of leaving them implicit when the class has any user-declared
  special member (rule of five/zero).
- Avoid `using namespace` outside of test files.

## Hot-path rules (queue push/pop and anything called per-message)

These rules apply specifically to code that runs on the enqueue/dequeue path
of the queue (and any future matching-engine-style hot loop), not to setup,
teardown, or test code:

- **No heap allocation.** No `new`, no growing `std::vector`, no
  `std::string` construction. Use fixed-size arrays (`std::array` or a raw
  `T[Capacity]`) sized at compile time, as `MPMCQueue` already does.
- **No exceptions.** Don't `throw` and don't call anything that can throw;
  mark hot-path functions `noexcept` so the compiler can drop unwind tables
  for them. Signal failure through return values (e.g. `try_push` returning
  `bool`) instead.
- **No locks, no blocking syscalls.** Synchronization is via `std::atomic`
  only. No `std::mutex`, no `std::condition_variable`, no I/O.
- **No virtual dispatch or RTTI.** Use templates/`constexpr if` for
  compile-time polymorphism instead of `virtual` functions on the hot path.
- **Avoid `std::shared_ptr`.** Its atomic refcounting adds overhead and false
  sharing; prefer value types or plain references/pointers with clear
  ownership.
- **Be deliberate about memory ordering.** Default to the weakest ordering
  that is still correct (`memory_order_relaxed`/`acquire`/`release`), not
  `memory_order_seq_cst`, but only after you can justify why it's safe.
  Comment non-obvious orderings with the invariant they protect — this is
  exactly the kind of "why" that's worth a comment.
- **Cache-line awareness.** Pad/align independently-written atomics (e.g.
  producer's `tail_` vs. consumer's `head_`) with `alignas(64)` (or whatever
  `detail::kCacheLineSize` resolves to) to avoid false sharing, as already
  done in `include/lfq/mpmc_queue.h`.
- **Prefer trivially-copyable payload types** (`T`) on the hot path. If a
  non-trivial `T` is required, be explicit about where it's constructed and
  destroyed inside the ring buffer.

## Testing & benchmarking mindset

- Don't trust intuition about lock-free performance — benchmark before and
  after any change to the synchronization strategy.
- Correctness bugs in lock-free code are usually ordering bugs, not logic
  bugs: when something misbehaves, suspect memory ordering and ABA-style
  issues before rewriting the algorithm.
