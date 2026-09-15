# lock-free-queue

A header-only, bounded multi-producer/multi-consumer (MPMC) lock-free queue
in C++17.

## What this is

This is a **Vyukov-style MPMC queue**: a fixed-capacity ring buffer where
each slot is a "cell" packing a value together with a sequence number into
one machine word, so a single atomic compare-and-swap can claim or release
a slot without any locks. The sequence number's low bit marks the cell
empty/full, and the rest of the bits track which lap around the ring the
cell is currently on — that's what lets producers and consumers coordinate
on the same fixed-size buffer without a mutex, a condition variable, or any
heap allocation on the push/pop/peek path.

The design follows the algorithm popularized by Dmitry Vyukov's
[bounded MPMC queue](https://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue),
with the packed value+sequence cell scheme inspired by
[erez-strauss/lockfree_mpmc_queue](https://github.com/erez-strauss/lockfree_mpmc_queue).

## Where this kind of queue is used

Bounded lock-free MPMC ring buffers like this one are a standard building
block anywhere multiple threads need to hand off work with predictable,
low, and bounded latency — no thread can be blocked waiting on a lock held
by another thread that got descheduled at the wrong time. Common places
this pattern shows up:

- **High-frequency trading / low-latency market data and order-entry
  systems** — passing market data ticks or order events between a network
  I/O thread and a strategy/matching thread with minimal, predictable
  jitter.
- **Audio and real-time media processing** — moving audio buffers between a
  real-time audio callback thread (which can never block or allocate) and
  a worker thread.
- **Game engines** — job systems and inter-thread task queues where worker
  threads pull work without lock contention stalling the frame.
- **High-throughput logging and telemetry** — application threads push log
  records into a queue without blocking on I/O; a dedicated writer thread
  drains it.
- General producer/consumer pipelines in any latency-sensitive service
  where garbage collection, blocking syscalls, or lock contention on the
  hot path are unacceptable.

## Testing

Every part of this project — `inplace_array`, `cell`, and `mpmc_queue`
(`push`/`pop`/`peek`) — has unit test coverage verifying its behavior,
including dedicated concurrency tests that hammer the queue with multiple
producer and consumer threads at once and check that every value is moved
exactly once, with none lost, duplicated, or corrupted. The concurrency
tests are also run under **ThreadSanitizer** (`-fsanitize=thread`) to catch
data races that can pass a normal test run by luck but are still real bugs
in the memory model. See `tests/` for the test suites and
`CLAUDE.md` for the project's sanitizer-usage guidelines.

## Building

```sh
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## On the use of AI in this project

I use AI (Claude Code) in this project for scaffolding boilerplate,
debugging, and generating unit tests. The queue design and the
implementation of the critical lock-free algorithms themselves — the cell
state machine and the push/pop/peek CAS retry loops — are my own work,
written by hand based on my own study of the relevant tutorials and
literature (see the references above). AI-assisted code review is used to
catch bugs in that hand-written logic (e.g. compile errors, data races,
and correctness edge cases), but the algorithmic design decisions are
mine.
