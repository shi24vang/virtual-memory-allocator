#ifndef ALLOCATOR_H
#define ALLOCATOR_H

#include <stddef.h>

typedef enum {
    ALLOC_STRATEGY_FIRST = 1,
    ALLOC_STRATEGY_NEXT,
    ALLOC_STRATEGY_BEST,
    ALLOC_STRATEGY_WORST,
    ALLOC_STRATEGY_BUDDY
} allocator_strategy_t;

#ifdef __cplusplus
extern "C" {
#endif

void* malloc_first_fit(size_t size);
void* malloc_next_fit(size_t size);
void* malloc_best_fit(size_t size);
void* malloc_worst_fit(size_t size);
void* malloc_buddy_alloc(size_t size);

void my_free(void *ptr);

allocator_strategy_t allocator_current_strategy(void);
const char* allocator_strategy_name(allocator_strategy_t strategy);

#ifdef __cplusplus
}
#endif

#endif
