#include "allocator.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef void* (*alloc_fn)(size_t);

static void smoke_alloc(const char *label, alloc_fn fn){
    int *buffer = fn(sizeof(int) * 8);
    assert(buffer && "allocation returned NULL");
    for (int i = 0; i < 8; ++i){
        buffer[i] = i * 17;
    }
    for (int i = 0; i < 8; ++i){
        assert(buffer[i] == i * 17);
    }
    my_free(buffer);
    printf("✓ %s allocator handled allocate/free cycle\n", label);
}

int main(void){
    smoke_alloc("first-fit", malloc_first_fit);
    smoke_alloc("next-fit", malloc_next_fit);
    smoke_alloc("best-fit", malloc_best_fit);
    smoke_alloc("worst-fit", malloc_worst_fit);

    char *buddy = malloc_buddy_alloc(512);
    assert(buddy && "buddy allocator returned NULL");
    strcpy(buddy, "buddy-ok");
    assert(strcmp(buddy, "buddy-ok") == 0);
    my_free(buddy);
    printf("✓ buddy allocator handled allocate/free cycle\n");

    puts("All allocator smoke tests passed.");
    return 0;
}
