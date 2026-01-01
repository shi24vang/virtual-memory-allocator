# Virtual Memory Allocator

User-space allocator that offers first-fit, next-fit, best-fit, worst-fit, and buddy strategies on top of `mmap`-backed arenas. No libc allocation calls are used inside the implementation.

## Overview
- Metadata for list-based fits lives in a single 4 KiB arena; a skip list speeds up best- and worst-fit searches.
- The next-fit allocator tracks a rover so scans resume from the previous position.
- Buddy allocation uses a separate 4 KiB arena with power-of-two free lists.
- Free blocks are stored in address order to make coalescing straightforward.

## Project Layout
- `include/allocator.h` – public API for all allocator entry points.
- `src/allocator.c` – arena setup, skip list maintenance, fit strategies, buddy allocator, and free logic.
- `examples/demo.c` – small CLI that exercises each strategy and prints what was used.
- `tests/basic_test.c` – smoke tests that allocate, write, and free under every strategy.

## Build & Run
```bash
make demo
./demo
```

## Testing
```bash
make test
```
