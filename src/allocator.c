/* Simple 4KB heap thing
 - Free list in address order, so neighbors easy to find + merge fast
 - Skip list for sizes, so best/worst fit ~log N (not too slow)
 - Next-fit use one “rover” pointer (like OSTEP say): start from i+1,
   if split happen then we go to the leftover part
 - Buddy alloc got its own 4KB area (we test that alone)
 Rules:
 - malloc_*: if size==0 or no space, just return NULL
 - my_free : no print error, if bad pointer or double free, just ignore quietly
 - Tiny tails: if after split the leftover too small (can’t hold header+MIN_TAIL),
   then give whole block to user (no tiny junk block left)
 Notes:
 - No malloc/free inside here; all meta info stay inside our arenas only
 - Skip list level use fixed-seed tiny PRNG (no libc rand), so same every run
let s go 
*/
#include "allocator.h"

#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

#define HEAP_SIZE 4096
#define MIN_TAIL  32

#define MAGIC_F   0xFEEDFACEU
#define MAGIC_A   0xDEADBEEFU


// this is used for debugging 
#ifdef MMU_DEBUG
  #include <assert.h>
  #define DBG(...) fprintf(stderr, "[mmu] " __VA_ARGS__)
#else
  #define DBG(...) ((void)0)
#endif

/* Free-block header (main heap
 * This header sit right before user data bytes.
 * It lives in two lists:
 *   1) Address list:  aprev <-> this <-> anext   (so we can merge neighbors)
 *   2) Size index:    snext[level]               (skip list for best/worst fit)
 */
#define SKLVL 6

typedef struct free_blk {
    size_t sz;                       // payload size in bytes
    struct free_blk *anext;          // thsi is address-ordered list (next/prev)
    struct free_blk *aprev;
    struct free_blk *snext[SKLVL];   // forward pointers of skip-list per level
    int lvl;                         // height 
    uint32_t magic;             
    uint8_t  is_free;                
} free_blk_t;

#define HDRSZ ((size_t)sizeof(free_blk_t))

/*Buddy header (separate arena)
Classic buddy: block size = 2^order. Only merges with exact buddy of same order
 */
typedef struct bud {
    size_t    sz;
    struct bud *next, *prev;         // free list per order
    uint32_t  magic;
    uint8_t   order;
    uint8_t   is_free;
} bud_t;

#define BUDHDR ((size_t)sizeof(bud_t))
#define MAXORD 13                    // initial BB = 2^(MAXORD-1)=4096

// Main code 
static void  *heap0      = NULL;
static void  *heap0_end  = NULL;
static int    heap0_inited = 0;

static free_blk_t *alist_head = NULL;    // address-sorted list head 
static free_blk_t *rover      = NULL;    // next-fit rover     

static int current_strategy = 0;

// Size-index 
static struct { free_blk_t *head[SKLVL]; } sidx;

// Buddy areaz
static void  *b_arena  = NULL;
static int    b_inited = 0;
static bud_t *bfl[MAXORD];

#define free_head    alist_head
#define size_index   sidx
#define next_rover   rover

static uint32_t prng_state = 0x9E3779B9U; // golden ratio seed 
static inline uint32_t xr(void){
    uint32_t x = prng_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    prng_state = x ? x : 0xA5A5A5A5U;
    return prng_state;
}
static int rand_lvl(void){
    // geometric p=1/2, capped at SKLVL 
    int h = 1;
    while (h < SKLVL && (xr() & 1U)) h++;
    return h;
}
static inline int cmp_size_addr(free_blk_t *a, free_blk_t *b){
    if (a->sz < b->sz) return -1;
    if (a->sz > b->sz) return  1;
    uintptr_t aa = (uintptr_t)a, bb = (uintptr_t)b;
    return (aa < bb) ? -1 : (aa > bb) ? 1 : 0;
}
static inline int adjacent(free_blk_t *a, free_blk_t *b){
    return ((char*)a + HDRSZ + a->sz) == (char*)b;
}
static void alu(free_blk_t *n){
    if (n->aprev) n->aprev->anext = n->anext; else alist_head = n->anext;
    if (n->anext) n->anext->aprev = n->aprev;
    n->aprev = n->anext = NULL;
}
static void alb(free_blk_t *prev, free_blk_t *next, free_blk_t *n){
    n->aprev = prev; n->anext = next;
    if (prev) prev->anext = n; else alist_head = n;
    if (next) next->aprev = n;
}
//Size-index (skip-list) ops
static void sidx_insert(free_blk_t *n){
    int L = n->lvl = rand_lvl();
    free_blk_t *upd[SKLVL]; for (int i=0;i<SKLVL;i++) upd[i]=NULL;
    // search the positions (>= by size,addr)
    free_blk_t *cur = NULL;
    for (int i=SKLVL-1;i>=0;i--){
        free_blk_t *p = (cur ? cur->snext[i] : sidx.head[i]);
        while (p && cmp_size_addr(p,n) < 0){ cur=p; p=p->snext[i]; }
        upd[i] = cur;
    }
    for (int i=0;i<L;i++){
        free_blk_t *p = upd[i] ? upd[i]->snext[i] : sidx.head[i];
        n->snext[i] = p;
        if (upd[i]) upd[i]->snext[i] = n; else sidx.head[i] = n;
    }
    for (int i=L;i<SKLVL;i++) n->snext[i] = NULL;
}
static void sidx_remove_exact(free_blk_t *n){
    free_blk_t *upd[SKLVL];
    free_blk_t *cur = NULL;
    for (int i=SKLVL-1;i>=0;i--){
        free_blk_t *p = (cur ? cur->snext[i] : sidx.head[i]);
        while (p && cmp_size_addr(p,n) < 0){ cur=p; p=p->snext[i]; }
        upd[i] = cur;
    }
    for (int i=0;i<SKLVL;i++){
        free_blk_t *next = upd[i] ? upd[i]->snext[i] : sidx.head[i];
        if (next == n){
            if (upd[i]) upd[i]->snext[i] = n->snext[i];
            else        sidx.head[i]     = n->snext[i];
        }
    }
}
// thsi is the first node with size >= need 
static free_blk_t* sidx_ge(size_t need){
    free_blk_t *cur = NULL;
    for (int i=SKLVL-1;i>=0;i--){
        free_blk_t *p = (cur ? cur->snext[i] : sidx.head[i]);
        while (p && p->sz < need){ cur=p; p=p->snext[i]; }
    }
    return cur ? cur->snext[0] : sidx.head[0];
}
// the largest node
static free_blk_t* sidx_max(void){
    free_blk_t *cur =NULL;
    for (int i=SKLVL-1;i>=0;i--){
        free_blk_t *p = (cur ? cur->snext[i] : sidx.head[i]);
        while (p){ cur=p; p=p->snext[i]; }
    }
    return cur;
}
static void heap_bootstrap(void){
    if (heap0_inited) return;

    void *p = mmap(NULL, HEAP_SIZE, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED){ perror("mmap"); _exit(1); }

    heap0 = p; heap0_end = (char*)p + HEAP_SIZE;
    for (int i=0;i<SKLVL;i++) sidx.head[i] = NULL;

    //. first (whole) free block 
    free_blk_t *b = (free_blk_t*)p;
    b->sz = HEAP_SIZE - HDRSZ;
    b->anext = b->aprev = NULL;
    for (int i=0;i<SKLVL;i++) b->snext[i]=NULL;
    b->lvl = 1;
    b->magic = MAGIC_F; b->is_free = 1;

    alist_head = b;
    sidx_insert(b);
    rover = b;                         
    prng_state = 0x9E3779B9U;        

    heap0_inited = 1;
}

/* 
 * If block big enough for need AND some tail left (>= HDRSZ + MIN_TAIL),
 * we cut front part for user and make new free tail block.
 * Else not worth it: return NULL and give full block to user,
 * so we don’t leave tiny useless piece in free list.
 */
static free_blk_t* smt(free_blk_t *blk, size_t need){
    size_t total  = HDRSZ + blk->sz;
    size_t needed = HDRSZ + need;
    if (total >= needed + HDRSZ + MIN_TAIL){
        free_blk_t *rem = (free_blk_t*)((char*)blk + needed);
        rem->sz = total - needed - HDRSZ;
        rem->anext = rem->aprev = NULL;
        for (int i=0;i<SKLVL;i++) rem->snext[i] = NULL;
        rem->lvl = 1;
        rem->magic = MAGIC_F; rem->is_free = 1;

        blk->sz = need;
        return rem;
    }
    return NULL;
}

/* Coalesce after we put a block back 
 * Try to join b with left and/or right neighbor if they sit next to it in mem.
 * After merge, put the new bigger block back into size index.
 * If rover was pointing to b or its neighbor, move rover to the merged block.
 */

static free_blk_t* cola(free_blk_t *b){
    free_blk_t *p = b->aprev, *n = b->anext;
    int merge_prev = (p && adjacent(p,b));
    int merge_next = (n && adjacent(b,n));
    if (!merge_prev && !merge_next) goto done;
    if (merge_prev) sidx_remove_exact(p);
    sidx_remove_exact(b);
    if (merge_next) sidx_remove_exact(n);
    if (merge_prev){
        p->anext = b->anext;
        if (b->anext) b->anext->aprev = p;
        p->sz += HDRSZ + b->sz;
        if (rover == b || rover == p) rover = p;
        b = p;
    }
    if (merge_next){
        free_blk_t *nn = n->anext;
        b->anext = nn; if (nn) nn->aprev = b;
        b->sz += HDRSZ + n->sz;
        if (rover == n || rover == b) rover = b;
    }
    sidx_insert(b);


done:
#ifdef MMU_DEBUG
    for (free_blk_t *q = alist_head; q && q->anext; q = q->anext){
        assert((uintptr_t)q < (uintptr_t)q->anext);
        assert(!adjacent(q, q->anext));
    }
#endif
    if (!alist_head) rover = NULL;   // safety to avoid dangling rover
    return b;
}
// important part 
// First-fit (O(N)): scan address list, split if helpful, re-index remainder
void* malloc_first_fit(size_t size){
    if (!size) return NULL;
    if (!heap0_inited) heap_bootstrap();
    current_strategy = ALLOC_STRATEGY_FIRST;

    for (free_blk_t *cur = alist_head; cur; cur = cur->anext){
        if (cur->sz >= size){
            free_blk_t *prev = cur->aprev, *next = cur->anext;
            alu(cur);
            sidx_remove_exact(cur);

            free_blk_t *rem = smt(cur, size);
            if (rem){
                alb(prev, next, rem);
                sidx_insert(rem);
                rover = rem;
            }else{
                rover = next ? next : alist_head;
            }
            cur->is_free = 0; cur->magic = MAGIC_A;
#ifdef MMU_DEBUG
            for (free_blk_t *q = alist_head; q && q->anext; q=q->anext)
                assert((uintptr_t)q < (uintptr_t)q->anext);
#endif
            return (char*)cur + HDRSZ;
        }
    }
    return NULL;
}
/* Next-fit (O(N)):
 * - Start from rover (or head if rover not set), walk around in circle.
 * - If we split a block, set rover = leftover part; else rover = next (wrap to head).
 * - If free list become empty (rare), set rover = NULL so it not point garbage.
*/
void* malloc_next_fit(size_t size){
    if (!size) return NULL;
    if (!heap0_inited) heap_bootstrap();
    current_strategy = ALLOC_STRATEGY_NEXT;
    if (!alist_head){ rover = NULL; return NULL; }
    if (!rover) rover = alist_head;
    free_blk_t *start = rover, *cur = start;
    do{
        if (cur->sz >= size){
            free_blk_t *prev = cur->aprev, *next = cur->anext;
            alu(cur);
            sidx_remove_exact(cur);
            free_blk_t *rem = smt(cur, size);
            if (rem){
                alb(prev, next, rem);
                sidx_insert(rem);
                rover = rem;                       
            }else{
                rover = alist_head ? (next ? next : alist_head) : NULL;
            }
            if (!alist_head) rover = NULL;         // for safety clamp 
            cur->is_free = 0; cur->magic = MAGIC_A;
            return (char*)cur + HDRSZ;
        }
        cur = cur->anext ? cur->anext : alist_head; 
    }while (cur && cur != start);

    if (!alist_head) rover = NULL;               // its too difficult boi ma man
    return NULL;
}

// Best-fit (O(log N)): pick smallest adequate from size index; split & re-index 
void* malloc_best_fit(size_t size){
    if (!size) return NULL;
    if (!heap0_inited) heap_bootstrap();
    current_strategy = ALLOC_STRATEGY_BEST;
    free_blk_t *best = sidx_ge(size);
    if (!best) return NULL;
    free_blk_t *prev = best->aprev, *next = best->anext;
    alu(best);
    sidx_remove_exact(best);
    free_blk_t *rem = smt(best, size);
    if (rem){ alb(prev, next, rem); sidx_insert(rem); }
    best->is_free = 0; best->magic = MAGIC_A;
    return (char*)best + HDRSZ;
}
// Worst-fit (O(log N)): pick largest from size index; split & re-index. 
void* malloc_worst_fit(size_t size){
    if (!size) return NULL;
    if (!heap0_inited) heap_bootstrap();
    current_strategy = ALLOC_STRATEGY_WORST;
    free_blk_t *w = sidx_max();
    if (!w || w->sz < size) return NULL;
    free_blk_t *prev = w->aprev, *next = w->anext;
    alu(w);
    sidx_remove_exact(w);
    free_blk_t *rem = smt(w, size);
    if (rem){ alb(prev, next, rem); sidx_insert(rem); }
    w->is_free = 0; w->magic = MAGIC_A;
    return (char*)w + HDRSZ;
}
// Buddy allocator
static void b_init(void){
    if (b_inited) return;
    void *p = mmap(NULL, HEAP_SIZE, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED){ perror("mmap(buddy)"); _exit(1); }
    b_arena = p;
    for (int i=0;i<MAXORD;i++) bfl[i]=NULL;
    bud_t *b = (bud_t*)p;
    b->sz = (size_t)1 << (MAXORD-1);
    b->order = MAXORD-1;
    b->magic = MAGIC_F; b->is_free = 1;
    b->next = b->prev = NULL;
    bfl[b->order] = b;
    b_inited = 1;
}
static bud_t* bgb(int order){
    int k = order;
    while (k < MAXORD && !bfl[k]) k++;
    if (k >= MAXORD) return NULL;
    bud_t *b = bfl[k];
    bfl[k] = b->next; if (b->next) b->next->prev = NULL;
    b->next = b->prev = NULL;
    while (k > order){
        k--;
        size_t half = (size_t)1 << k;
        bud_t *L = b;
        bud_t *R = (bud_t*)((char*)b + half);
        L->sz = R->sz = half;
        L->order = R->order = (uint8_t)k;
        L->magic = R->magic = MAGIC_F;
        L->is_free = R->is_free = 1;
        R->next = bfl[k]; R->prev = NULL;
        if (bfl[k]) bfl[k]->prev = R;
        bfl[k] = R;
        b = L;
    }
    b->is_free = 0; b->magic = MAGIC_A;
    return b;
}
static inline bud_t* b_buddy(bud_t *b){
    size_t sz = (size_t)1 << b->order;
    uintptr_t off  = (uintptr_t)((char*)b - (char*)b_arena);
    uintptr_t boff = off ^ sz;
    return (boff < HEAP_SIZE) ? (bud_t*)((char*)b_arena + boff) : NULL;
}
static void bfm(bud_t *b){
    b->is_free = 1; b->magic = MAGIC_F;
    b->next = bfl[b->order]; b->prev = NULL;
    if (bfl[b->order]) bfl[b->order]->prev = b;
    bfl[b->order] = b;
    while (b->order < MAXORD-1){
        bud_t *m = b_buddy(b);
        if (!m || !m->is_free || m->order != b->order) break;
        if (m->prev) m->prev->next = m->next;
        else         bfl[m->order] = m->next;
        if (m->next) m->next->prev = m->prev;
        // unlink b (currently in free list head)
        if (b->prev) b->prev->next = b->next;
        else         bfl[b->order] = b->next;
        if (b->next) b->next->prev = b->prev;
        // merged block starts at lower address of the pair
        b = ((uintptr_t)m < (uintptr_t)b) ? m : b;
        b->order++; b->sz <<= 1;
        b->prev = b->next = NULL;
        b->next = bfl[b->order]; b->prev = NULL;
        if (bfl[b->order]) bfl[b->order]->prev = b;
        bfl[b->order] = b;
    }
}
void* malloc_buddy_alloc(size_t size){
    if (!size) return NULL;
    if (!b_inited) b_init();
    current_strategy = ALLOC_STRATEGY_BUDDY;

    size_t need = size + BUDHDR;
    int order = 0; size_t blk = 1;
    while (blk < need && order < MAXORD){ blk <<= 1; order++; }
    if (order >= MAXORD) return NULL;
    bud_t *b = bgb(order);
    if (!b) return NULL;
    return (char*)b + BUDHDR;
}
/* Free
 * Put block back in address order list, also add it to size index, then try merge with neighbors.
 * Error rule: if bad pointer or double free, just return quiet (no print).
 */
void my_free(void *ptr){
    if (!ptr) return;
    // the Buddy pointer////
    if (b_inited){
        uintptr_t p  = (uintptr_t)ptr;
        uintptr_t b0 = (uintptr_t)b_arena, b1 = b0 + HEAP_SIZE;
        if (p >= b0 && p < b1){
            bud_t *b = (bud_t*)((char*)ptr - BUDHDR);
            if (b->magic != MAGIC_A) return;  // this will get  silent on invalid
            bfm(b);
            return;
        }
    }
    if (!heap0_inited) return;
    free_blk_t *blk = (free_blk_t*)((char*)ptr - HDRSZ);
    if (blk->magic != MAGIC_A) return;       // this will get  silent on invalidd
    // Insert by address
    if (!alist_head){
        alist_head = blk; blk->aprev = blk->anext = NULL;
    }else{
        free_blk_t *cur = alist_head, *prv = NULL;
        while (cur && (uintptr_t)cur < (uintptr_t)blk){ prv = cur; cur = cur->anext; }
        alb(prv, cur, blk);
    }
    blk->is_free = 1; blk->magic = MAGIC_F;
    for (int i=0;i<SKLVL;i++) blk->snext[i]=NULL;
    blk->lvl = 1;
    sidx_insert(blk);

    (void)cola(blk);              // rover might be updated inside 
#ifdef MMU_DEBUG
    for (free_blk_t *q = alist_head; q && q->anext; q=q->anext){
        assert((uintptr_t)q < (uintptr_t)q->anext);
        assert(!adjacent(q, q->anext));
    }
#endif
}

allocator_strategy_t allocator_current_strategy(void){
    if (current_strategy >= ALLOC_STRATEGY_FIRST &&
        current_strategy <= ALLOC_STRATEGY_BUDDY){
        return (allocator_strategy_t)current_strategy;
    }
    return ALLOC_STRATEGY_FIRST;
}

const char* allocator_strategy_name(allocator_strategy_t strategy){
    switch (strategy){
        case ALLOC_STRATEGY_FIRST: return "first-fit";
        case ALLOC_STRATEGY_NEXT:  return "next-fit";
        case ALLOC_STRATEGY_BEST:  return "best-fit";
        case ALLOC_STRATEGY_WORST: return "worst-fit";
        case ALLOC_STRATEGY_BUDDY: return "buddy";
        default:                   return "unknown";
    }
}
