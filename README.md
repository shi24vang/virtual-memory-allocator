# Virtual Memory Allocator

Custom user-space allocator that mimics an OS-style virtual memory manager. It exposes dedicated entry points for first-fit, next-fit, best-fit, worst-fit, and buddy strategies while keeping all metadata inside `mmap`-backed arenas.

## Feature Highlights
- **mmap-managed arenas** – allocator never falls back to the C runtime; metadata lives inside private heaps.
- **Custom allocation APIs** – each fit strategy is its own entry point so experiments can switch policies at call sites.
- **Dual data structures** – address-ordered free list enables O(1) coalescing; a size-indexed skip list keeps best/worst-fit lookups about `O(log N)`.
- **Rover-based next fit** – stateful pointer resumes scanning where it left off, matching textbook OS behavior.
- **Dedicated buddy allocator** – separate 4 KiB arena for clear fragmentation comparisons against list-based fits.
- **Deterministic skip-list heights** – a small XOR-shift PRNG keeps structure choices reproducible during profiling.

## Architecture Overview
| Strategy  | Data structure            | Notes                                              |
|-----------|---------------------------|----------------------------------------------------|
| First fit | Address-sorted free list  | Linear scan; merges happen immediately on free.    |
| Next fit  | Address-sorted list + rover | Continues from last position; rover updated on split/merge. |
| Best fit  | Skip list keyed by size/address | Finds smallest adequate block in logarithmic time. |
| Worst fit | Skip list keyed by size/address | Pulls largest block to reduce fragmentation experiments. |
| Buddy     | Power-of-two free lists   | Classic buddy logic with constant-time buddy lookup. |

The entire allocator lives in `src/allocator.c`. Every strategy shares the same metadata layout, so switching policies is purely a matter of which search primitive is called.

## Project Layout
- `include/allocator.h` – public API surface with the strategy enum.
- `src/allocator.c` – arena initialization, skip-list maintenance, fit strategies, buddy logic, and diagnostics helpers.
- `examples/demo.c` – CLI showcase that runs each strategy and prints the active policy.
- `tests/basic_test.c` – smoke test that allocates, writes, and frees memory under every strategy.

## Profiling & Fragmentation Analysis
- **Synthetic workloads**: swap in different allocation traces within `tests/` (or your own harness) to drive steady-state loads, bursty spikes, or random workloads. Each strategy is a single function pointer, so it is straightforward to re-run the same trace under multiple policies.
- **Latency hooks**: wrap the exposed APIs with `clock_gettime` counters to collect per-allocation latency; the allocator keeps metadata inside the arenas, so instrumentation overhead is the only variable you add.
- **Fragmentation metrics**: the skip-list orders blocks by size, making it easy to walk the structure and compute external fragmentation or variance per workload. Buddy stats can be collected by reading the per-order free lists.
- **Heap tuning**: tweak `HEAP_SIZE`, `MIN_TAIL`, or `MAXORD` and re-run your trace to evaluate how arena sizing impacts latency vs. fragmentation.

## Build & Run
```bash
make demo        # builds the static library and demo binary
./demo
```

Sample output:
```
=== best-fit ===
 block A payload preview: AAAAAAAAAAAAAAAA...
 block B payload preview: bbbbbbbbbbbbbbbb...
 strategy recorded as: best-fit
```

## Testing
```bash
make test
```
