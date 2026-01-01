#include "allocator.h"

#include <stdio.h>
#include <string.h>

typedef void* (*alloc_fn)(size_t);

typedef struct {
    const char *label;
    alloc_fn    fn;
} strategy_case;

static void run_strategy(const strategy_case *scase){
    printf("=== %s ===\n", scase->label);

    char *a = scase->fn(128);
    char *b = scase->fn(64);
    if (!a || !b){
        printf("allocation failed\n\n");
        if (a) my_free(a);
        if (b) my_free(b);
        return;
    }

    memset(a, 'A', 127); a[127] = '\0';
    memset(b, 'b', 63);  b[63]  = '\0';

    printf(" block A payload preview: %.16s...\n", a);
    printf(" block B payload preview: %.16s...\n", b);
    printf(" strategy recorded as: %s\n\n",
           allocator_strategy_name(allocator_current_strategy()));

    my_free(a);
    my_free(b);
}

int main(void){
    const strategy_case cases[] = {
        {"first-fit", malloc_first_fit},
        {"next-fit",  malloc_next_fit},
        {"best-fit",  malloc_best_fit},
        {"worst-fit", malloc_worst_fit}
    };

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i){
        run_strategy(&cases[i]);
    }

    printf("=== buddy allocator ===\n");
    char *buddy = malloc_buddy_alloc(256);
    if (buddy){
        strcpy(buddy, "Buddy blocks are power-of-two sized!");
        printf(" buddy block: %s\n", buddy);
        my_free(buddy);
    }else{
        printf(" buddy allocation failed\n");
    }
    return 0;
}
