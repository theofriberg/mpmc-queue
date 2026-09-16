#include "lfq/mpmc_queue.h"

#include <memory>

#include <benchmark/benchmark.h>

namespace {

constexpr std::size_t kCapacity = 4096;
using queue_type = lfq::mpmc_queue<int, kCapacity>;

// Baseline: push() immediately followed by pop() on a single thread, no
// contention at all. This measures the pure cost of one CAS retry loop
// through the happy path on each side -- the floor that any multi-threaded
// number will sit above.
void BM_PushPopRoundTrip(benchmark::State& state) {
    queue_type q;
    int value = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(q.push(42));
        benchmark::DoNotOptimize(q.pop(value));
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_PushPopRoundTrip);

// Single-producer/single-consumer: two real threads sharing one queue,
// thread 0 pushing and thread 1 popping as fast as they can. This is the
// simplest scenario with genuine cross-thread contention -- both threads
// touch tail_/head_ (separate cache lines) and race on the same cells at
// the wraparound boundary, but there's no producer-vs-producer or
// consumer-vs-consumer CAS racing yet (that's the NPMC benchmark to add
// later).
void BM_SpscThroughput(benchmark::State& state) {
    static std::unique_ptr<queue_type> q;

    // Google Benchmark places a barrier at loop entry/exit for all threads
    // in a ->Threads(N) run, so this thread-0-only construction is visible
    // to thread 1 by the time it reaches its own loop.
    if (state.thread_index() == 0) {
        q = std::make_unique<queue_type>();
    }

    if (state.thread_index() == 0) {
        for (auto _ : state) {
            while (!q->push(42)) {
                // Backpressure: consumer hasn't caught up, spin and retry.
            }
        }
    } else {
        int value = 0;
        for (auto _ : state) {
            while (!q->pop(value)) {
                // Nothing produced yet, spin and retry.
            }
        }
    }

    if (state.thread_index() == 0) {
        state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
        q.reset();
    }
}
BENCHMARK(BM_SpscThroughput)->Threads(2)->UseRealTime();

}  // namespace

BENCHMARK_MAIN();
