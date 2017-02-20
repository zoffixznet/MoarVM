#include "moar.h"
#include "internal.h"

#define __COMMA__ ,
static MVMint8 available_gpr[] = {
    MVM_JIT_ARCH_AVAILABLE_GPR(MVM_JIT_REG)
};
static MVMint8 available_num[] = {
    MVM_JIT_ARCH_NUM(MVM_JIT_REG)
};
/* bitmap, so make it '|' to combine the shifted register numbers */
#undef __COMMA__
#define __COMMA__ |
#define SHIFT(x) (1 << (MVM_JIT_REG(x)))
static const MVMint64 NVR_GPR_BITMAP = MVM_JIT_ARCH_NONVOLATILE_GPR(SHIFT);
#undef SHIFT
#undef __COMMA__


#define MAX_ACTIVE sizeof(available_gpr)
#define NYI(x) MVM_oops(tc, #x  "not yet implemented")

typedef struct {
    MVMint32 key;
    MVMint32 idx;
} UnionFind;


#ifdef MVM_JIT_DEBUG
#define _DEBUG(...) fprintf(stderr, __VA_ARGS__)
#else
#define _DEBUG(...) do {} while(0)
#endif


typedef struct ValueRef ValueRef;
struct ValueRef {
    MVMint32  tile_idx;
    MVMint32  value_idx;
    ValueRef *next;
};

typedef struct {
    /* double-ended queue of value refs */
    ValueRef *first, *last;

    /* We can have at most two synthetic tiles, one attached to the first
     * definition and one to the last use... we could also point directly into
     * the values array of the tile, but it is not directly necessary */
    MVMint32    synth_pos[2];
    MVMJitTile *synthetic[2];

    MVMint8            register_spec;
    MVMJitStorageClass reg_cls;
    MVMint32           reg_num;

    MVMint32           spill_pos;
} LiveRange;




typedef struct {
    MVMJitCompiler *compiler;

    /* Sets of values */
    UnionFind *sets;

    /* single buffer for uses, definitions */
    ValueRef *refs;
    MVMint32  refs_num;

    /* All values ever defined by the register allcoator */
    MVM_VECTOR_DECL(LiveRange, values);

    /* 'Currently' active values */
    MVMint32 active_top;
    MVMint32 active[MAX_ACTIVE];

    /* Values still left to do (heap) */
    MVM_VECTOR_DECL(MVMint32, worklist);
    /* Retired values (to be assigned registers) (heap) */
    MVM_VECTOR_DECL(MVMint32, retired);
    /* Spilled values */
    MVM_VECTOR_DECL(MVMint32, spilled);


    /* Register handout ring */
    MVMint8   reg_ring[MAX_ACTIVE];
    MVMint32  reg_give, reg_take;

} RegisterAllocator;


/* For first/last ref comparison, the tile indexes are doubled, and the indexes
 * of synthetics are biased with +1/-1. We use this extra space on the number
 * line to ensure consistent ordering and expiring behavior for 'synthetic' live
 * ranges that either start before an instruction (loading a required value) or
 * end just after one (storing the produced value). Without this, ordering
 * problems can cause two 'atomic' live ranges to be allocated and expired
 * before their actual last use */
static inline MVMint32 order_nr(MVMint32 tile_idx) {
    return tile_idx * 2;
}

/* quick accessors for common checks */
static inline MVMint32 first_ref(LiveRange *r) {
    MVMint32 a = r->first == NULL        ? INT32_MAX : order_nr(r->first->tile_idx);
    MVMint32 b = r->synthetic[0] == NULL ? INT32_MAX : order_nr(r->synth_pos[0]) - 1;
    return MIN(a,b);
}

static inline MVMint32 last_ref(LiveRange *r) {
    MVMint32 a = r->last == NULL         ? -1 : order_nr(r->last->tile_idx);
    MVMint32 b = r->synthetic[1] == NULL ? -1 : order_nr(r->synth_pos[1]) + 1;
    return MAX(a,b);
}

static inline MVMint32 is_definition(ValueRef *v) {
    return (v->value_idx == 0);
}

static inline MVMint32 is_arglist_ref(MVMJitTileList *list, ValueRef *v) {
    return (list->items[v->tile_idx]->op == MVM_JIT_ARGLIST);
}

/* allocate a new live range value by pointer-bumping */
MVMint32 live_range_init(RegisterAllocator *alc) {
    LiveRange *range;
    MVMint32 idx = alc->values_num++;
    MVM_VECTOR_ENSURE_SIZE(alc->values, idx);
    return idx;
}

static inline MVMint32 live_range_is_empty(LiveRange *range) {
    return (range->first == NULL &&
            range->synthetic[0] == NULL &&
            range->synthetic[1] == NULL);
}


/* append ref to end of queue */
static void live_range_add_ref(RegisterAllocator *alc, LiveRange *range, MVMint32 tile_idx, MVMint32 value_idx) {
    ValueRef *ref = alc->refs + alc->refs_num++;

    ref->tile_idx  = tile_idx;
    ref->value_idx = value_idx;

    if (range->first == NULL) {
        range->first = ref;
    }
    if (range->last != NULL) {
        range->last->next = ref;
    }
    range->last = ref;
    ref->next   = NULL;
}

/* merge value ref sets */
static void live_range_merge(LiveRange *a, LiveRange *b) {
    ValueRef *head = NULL, *tail = NULL;
    MVMint32 i;
    _DEBUG("Merging live ranges (%d-%d) and (%d-%d)\n",
           first_ref(a), last_ref(a), first_ref(b), last_ref(b));
    if (first_ref(a) <= first_ref(b)) {
        head = a->first;
        a->first = a->first->next;
    } else {
        head = b->first;
        b->first = b->first->next;
    }
    tail = head;
    while (a->first != NULL && b->first != NULL) {
        if (a->first->tile_idx <= b->first->tile_idx) {
            tail->next  = a->first;
            a->first    = a->first->next;
        } else {
            tail->next  = b->first;
            b->first    = b->first->next;
        }
        tail = tail->next;
    }
    while (a->first != NULL) {
        tail->next = a->first;
        a->first   = a->first->next;
        tail       = tail->next;
    }
    while (b->first != NULL) {
        tail->next  = b->first;
        b->first    = b->first->next;
        tail        = tail->next;
    }

    a->first = head;
    a->last  = tail;

    for (i = 0; i < 2; i++) {
        if (b->synthetic[i] == NULL) {
            continue;
        }
        if (a->synthetic[i] != NULL) {
            MVM_panic(1, "Can't merge the same synthetic!");
        }
        a->synthetic[i] = b->synthetic[i];
        a->synth_pos[i] = b->synth_pos[i];
    }
}



UnionFind * value_set_find(UnionFind *sets, MVMint32 key) {
    while (sets[key].key != key) {
        key = sets[key].key;
    }
    return sets + key;
}

MVMint32 value_set_union(UnionFind *sets, LiveRange *values, MVMint32 a, MVMint32 b) {

    /* dereference the sets to their roots */
    a = value_set_find(sets, a)->key;
    b = value_set_find(sets, b)->key;
    if (a == b) {
        /* secretly the same set anyway, could happen in some combinations of
         * IF, COPY, and DO. */
        return a;
    }
    if (first_ref(values + sets[b].idx) < first_ref(values + sets[a].idx)) {
        /* ensure we're picking the first one to start so that we maintain the
         * first-definition heap order */
        MVMint32 t = a; a = b; b = t;
    }
    sets[b].key = a; /* point b to a */
    live_range_merge(values + sets[a].idx, values + sets[b].idx);
    return a;
}


static inline void heap_swap(MVMint32 *heap, MVMint32 a, MVMint32 b) {
    MVMint32 t = heap[a];
    heap[a]    = heap[b];
    heap[b]    = t;
}

/* Functions to maintain a heap of references to the live ranges */
void live_range_heap_down(LiveRange *values, MVMint32 *heap, MVMint32 top, MVMint32 item) {
    while (item < top) {
        MVMint32 left = item * 2 + 1;
        MVMint32 right = left + 1;
        MVMint32 swap;
        if (right < top) {
            swap = first_ref(&values[heap[left]]) < first_ref(&values[heap[right]]) ? left : right;
        } else if (left < top) {
            swap = left;
        } else {
            break;
        }
        if (first_ref(&values[heap[swap]]) < first_ref(&values[heap[item]])) {
            heap_swap(heap, swap, item);
            item       = swap;
        } else {
            break;
        }
    }
}

void live_range_heap_up(LiveRange *values, MVMint32 *heap, MVMint32 item) {
    while (item > 0) {
        MVMint32 parent = (item-1)/2;
        if (first_ref(&values[heap[parent]]) > first_ref(&values[heap[item]])) {
            heap_swap(heap, item, parent);
            item = parent;
        } else {
            break;
        }
    }
}

MVMint32 live_range_heap_pop(LiveRange *values, MVMint32 *heap, size_t *top) {
    MVMint32 v = heap[0];
    MVMint32 t = --(*top);
    /* pop by swap and heap-down */
    heap[0]    = heap[t];
    live_range_heap_down(values, heap, t, 0);
    return v;
}

void live_range_heap_push(LiveRange *values, MVMint32 *heap, size_t *top, MVMint32 v) {
    /* NB, caller should use MVM_ENSURE_SPACE prior to calling */
    MVMint32 t = (*top)++;
    heap[t] = v;
    live_range_heap_up(values, heap, t);
}

void live_range_heapify(LiveRange *values, MVMint32 *heap, MVMint32 top) {
    MVMint32 i = top, mid = top/2;
    while (i-- > mid) {
        live_range_heap_up(values, heap, i);
    }
}


/* register assignment logic */
#define ARRAY_SIZE(a) (sizeof(a)/sizeof(a[0]))
#define NEXT_IN_RING(a,x) (((x)+1) == ARRAY_SIZE(a) ? 0 : ((x)+1))
MVMint8 get_register(MVMThreadContext *tc, RegisterAllocator *alc, MVMJitStorageClass reg_cls) {
    /* ignore storage class for now */
    MVMint8 reg_num;
    reg_num       = alc->reg_ring[alc->reg_take];
    if (reg_num >= 0) {
        /* not empty */
        alc->reg_ring[alc->reg_take] = -1; /* mark used */
        alc->reg_take = NEXT_IN_RING(alc->reg_ring, alc->reg_take);
    }
    return reg_num;
}

void free_register(MVMThreadContext *tc, RegisterAllocator *alc, MVMJitStorageClass reg_cls, MVMint8 reg_num) {
    if (alc->reg_ring[alc->reg_give] != -1) {
        MVM_oops(tc, "No space to release register %d to ring", reg_num);
    }
    alc->reg_ring[alc->reg_give] = reg_num;
    alc->reg_give = NEXT_IN_RING(alc->reg_ring, alc->reg_give);
}

void assign_register(MVMThreadContext *tc, RegisterAllocator *alc, MVMJitTileList *list,
                     MVMint32 lv, MVMJitStorageClass reg_cls,  MVMint8 reg_num) {
    /* What to do here:
     * - update tiles using this live range to refer to this register
     * - update allocator to mark this register as used by this live range */
    LiveRange *range = alc->values + lv;
    ValueRef *ref;
    MVMint32 i;

    range->reg_cls   = reg_cls;
    range->reg_num   = reg_num;
    for (ref = range->first; ref != NULL; ref = ref->next) {
        if (is_arglist_ref(list, ref)) {
            /* don't assign registers to ARGLIST references, that will never
             * work */
            continue;
        }
        MVMJitTile *tile = list->items[ref->tile_idx];
        tile->values[ref->value_idx] = reg_num;
    }

    for (i = 0; i < 2; i++) {
        MVMJitTile *tile = range->synthetic[i];
        if (tile != NULL) {
            tile->values[i] = reg_num;
        }
    }
}


static void determine_live_ranges(MVMThreadContext *tc, RegisterAllocator *alc, MVMJitTileList *list) {
    MVMJitExprTree *tree = list->tree;
    MVMint32 i, j;

    alc->sets = MVM_calloc(tree->nodes_num, sizeof(UnionFind));
    /* up to 4 refs per tile (1 out, 3 in) plus the number of refs per arglist */
    alc->refs = MVM_calloc(list->items_num * 4 + list->num_arglist_refs, sizeof(ValueRef));
    alc->refs_num = 0;

    MVM_VECTOR_INIT(alc->values,   list->items_num);
    MVM_VECTOR_INIT(alc->worklist, list->items_num);

    for (i = 0; i < list->items_num; i++) {
        MVMJitTile *tile = list->items[i];
        MVMint32    node = tile->node;
        /* Each of the following counts as either an alias or as a PHI (in case
         * of IF), and thus these are not actual definitions */
        if (tile->op == MVM_JIT_COPY) {
            MVMint32 ref        = tree->nodes[tile->node + 1];
            alc->sets[node].key = ref; /* point directly to actual definition */
        } else if (tile->op == MVM_JIT_DO && MVM_JIT_TILE_YIELDS_VALUE(tile)) {
            MVMint32 nchild     = tree->nodes[tile->node + 1];
            MVMint32 ref        = tree->nodes[tile->node + nchild];
            alc->sets[node].key = ref;
        } else if (tile->op == MVM_JIT_IF) {
            MVMint32 left_cond   = tree->nodes[tile->node + 2];
            MVMint32 right_cond  = tree->nodes[tile->node + 3];
            /* NB; this may cause a conflict, in which case we can resolve it by
             * creating a new live range or inserting a copy */
            alc->sets[node].key  = value_set_union(alc->sets, alc->values, left_cond, right_cond);
        } else if (tile->op == MVM_JIT_ARGLIST) {
            MVMint32 num_args = list->tree->nodes[tile->node + 1];
            MVMJitExprNode *refs = list->tree->nodes + tile->node + 2;
            for (j = 0; j < num_args; j++) {
                MVMint32 carg  = refs[j];
                MVMint32 value = list->tree->nodes[carg+1];
                MVMint32 idx   = value_set_find(alc->sets, value)->idx;
                live_range_add_ref(alc, alc->values + idx, i, j + 1);
            }
        } else {
            /* create a live range if necessary */
            if (MVM_JIT_TILE_YIELDS_VALUE(tile)) {
                MVMint8 register_spec = MVM_JIT_REGISTER_FETCH(tile->register_spec, 0);
                MVMint32 idx          = live_range_init(alc);
                alc->sets[node].key   = node;
                alc->sets[node].idx   = idx;
                live_range_add_ref(alc, alc->values + idx, i, 0);
                if (MVM_JIT_REGISTER_HAS_REQUIREMENT(register_spec)) {
                    alc->values[idx].register_spec = register_spec;
                }
                MVM_VECTOR_PUSH(alc->worklist, idx);
            }
            /* account for uses */
            for (j = 0; j < tile->num_refs; j++) {
                MVMint8  register_spec = MVM_JIT_REGISTER_FETCH(tile->register_spec, j+1);
                /* any 'use' register requirements are handled in the allocation step */
                if (MVM_JIT_REGISTER_IS_USED(register_spec)) {
                    MVMint32 idx = value_set_find(alc->sets, tile->refs[j])->idx;
                    live_range_add_ref(alc, alc->values + idx, i, j + 1);
                }
            }
        }
    }

}

/* The code below needs some thinking... */
static void active_set_add(MVMThreadContext *tc, RegisterAllocator *alc, MVMint32 a) {
    /* the original linear-scan heuristic for spilling is to take the last value
     * in the set to expire, freeeing up the largest extent of code... that is a
     * reasonably good heuristic, albeit not essential to the concept of linear
     * scan. It makes sense to keep the stack ordered at all times (simplest by
     * use of insertion sort). Although insertion sort is O(n^2), n is never
     * large in this case (32 for RISC architectures, maybe, if we ever support
     * them; 7 for x86-64. So the time spent on insertion sort is always small
     * and bounded by a constant, hence O(1). Yes, algorithmics works this way
     * :-) */
    MVMint32 i;
    for (i = 0; i < alc->active_top; i++) {
        MVMint32 b = alc->active[i];
        if (last_ref(&alc->values[b]) > last_ref(&alc->values[a])) {
            /* insert a before b */
            memmove(alc->active + i + 1, alc->active + i, sizeof(MVMint32)*(alc->active_top - i));
            alc->active[i] = a;
            alc->active_top++;
            return;
        }
    }
    /* append at the end */
    alc->active[alc->active_top++] = a;
}



/* Take live ranges from active_set whose last use was after position and append them to the retired list */
static void active_set_expire(MVMThreadContext *tc, RegisterAllocator *alc, MVMint32 order_nr) {
    MVMint32 i;
    for (i = 0; i < alc->active_top; i++) {
        MVMint32 v = alc->active[i];
        MVMint8 reg_num = alc->values[v].reg_num;
        if (last_ref(&alc->values[v]) > order_nr) {
            break;
        } else {
            _DEBUG("Live range %d is out of scope (last ref %d, %d) and releasing register %d\n",
                    v, last_ref(alc->values + v), position, reg_num);
            free_register(tc, alc, MVM_JIT_STORAGE_GPR, reg_num);
        }
    }

    /* shift off the first x values from the live set. */
    if (i > 0) {
        MVM_VECTOR_APPEND(alc->retired, alc->active, i);
        MVM_VECTOR_SHIFT(alc->active, i);
    }
}

static void active_set_splice(MVMThreadContext *tc, RegisterAllocator *alc, MVMint32 to_splice) {
    MVMint32 i ;
    /* find (reverse, because it's usually the last); predecrement alc->active_top */
    for (i = --alc->active_top; i >= 0; i--) {
        if (alc->active[i] == to_splice)
            break;
    }
    if (i >= 0 && i < alc->active_top) {
        /* shift out */
        memmove(alc->active + i, alc->active + i + 1,
                sizeof(alc->active[0]) * alc->active_top - i);
    }
}



static MVMint32 insert_load_before_use(MVMThreadContext *tc, RegisterAllocator *alc, MVMJitTileList *list,
                                       ValueRef *ref, MVMint32 load_pos) {
    MVMint32 n = live_range_init(alc);
    MVMJitTile *tile = MVM_jit_tile_make(tc, alc->compiler, MVM_jit_compile_load, -1, 1, load_pos);
    MVM_jit_tile_list_insert(tc, list, tile, ref->tile_idx - 1, +1); /* insert just prior to use */
    alc->values[n].synthetic[0] = tile;
    alc->values[n].synth_pos[0] = ref->tile_idx;
    alc->values[n].first = alc->values[n].last = ref;
    return n;
}

static MVMint32 insert_store_after_definition(MVMThreadContext *tc, RegisterAllocator *alc, MVMJitTileList *list,
                                              ValueRef *ref, MVMint32 store_pos) {
    MVMint32 n       = live_range_init(alc);
    MVMJitTile *tile = MVM_jit_tile_make(tc, alc->compiler, MVM_jit_compile_store, -1, 1, store_pos);
    MVM_jit_tile_list_insert(tc, list, tile, ref->tile_idx, -1); /* insert just after storage */
    alc->values[n].synthetic[1] = tile;
    alc->values[n].synth_pos[1] = ref->tile_idx;
    alc->values[n].first = alc->values[n].last = ref;
    return n;
}

static MVMint32 select_live_range_for_spill(MVMThreadContext *tc, RegisterAllocator *alc, MVMJitTileList *list, MVMint32 code_pos) {
    return alc->active[alc->active_top-1];
}

static MVMint32 select_memory_for_spill(MVMThreadContext *tc, RegisterAllocator *alc, MVMJitTileList *list,
                                        MVMint32 code_pos, MVMuint32 size) {
    /* TODO: Implement a 'free list' of spillable locations */
    MVMint32 pos = alc->compiler->spill_top;
    alc->compiler->spill_top += size;
    return pos;
}


static void live_range_spill(MVMThreadContext *tc, RegisterAllocator *alc, MVMJitTileList *list,
                             MVMint32 to_spill, MVMint32 spill_pos, MVMint32 code_pos) {
    LiveRange *spillee  = alc->values + to_spill;
    MVMint8 reg_spilled = spillee->reg_num;
    /* loop over all value refs */
    ValueRef **head     = &(spillee->first);
    while (*head != NULL) {
        /* make a new live range */
        MVMint32 n;
        /* shift current ref */
        ValueRef *ref = *head;
        *head         = ref->next;
        ref->next     = NULL;
        if (is_arglist_ref(list, ref) && order_nr(ref->tile_idx) > code_pos) {
            /* Never insert a load before a future ARGLIST; ARGLIST may easily
             * consume more registers than we have available. Past ARGLISTs have
             * already been handled, so we do need to insert a load a before
             * them (or modify in place, but, complex!). */
            continue;
        } else if (is_definition(ref)) {
            n = insert_store_after_definition(tc, alc, list, ref, spill_pos);
        } else {
            n = insert_load_before_use(tc, alc, list, ref, spill_pos);
        }

        if (order_nr(ref->tile_idx) < code_pos) {
            /* in the past, which means we can safely use the spilled register
             * and immediately retire this live range */
            assign_register(tc, alc, list, n, MVM_JIT_STORAGE_GPR, reg_spilled);
            MVM_VECTOR_PUSH(alc->retired, n);
        } else {
            /* in the future, which means we need to add it to the worklist */
            MVM_VECTOR_ENSURE_SPACE(alc->worklist, 1);
            live_range_heap_push(alc->values, alc->worklist, &alc->worklist_num, n);
        }
    }
    /* mark as spilled and store the spill position */
    spillee->spill_pos = spill_pos;
    free_register(tc, alc, MVM_JIT_STORAGE_GPR, reg_spilled);
    MVM_VECTOR_PUSH(alc->spilled, to_spill);
}

static void spill_any_register(MVMThreadContext *tc, RegisterAllocator *alc, MVMJitTileList *list, MVMint32 code_position) {
    /* choose a live range, a register to spill, and a spill location */
    MVMint32 to_spill   = select_live_range_for_spill(tc, alc, list, code_position);
    MVMint32 spill_pos  = select_memory_for_spill(tc, alc, list, code_position, sizeof(MVMRegister));
    active_set_splice(tc, alc, to_spill);
    live_range_spill(tc, alc, list, to_spill, spill_pos, code_position);
}


/* not sure if this is sufficiently general-purpose and unconfusing */
#define MVM_VECTOR_ASSIGN(a,b) do {             \
        a = b;                                  \
        a ## _top = b ## _top;                  \
        a ## _alloc = b ## _alloc;              \
    } while (0);

static void compile_arglist_node(MVMThreadContext *tc, RegisterAllocator *alc, MVMJitTileList *list, MVMint32 tile_idx) {
}

static void spill_over_call_node(MVMThreadContext *tc, RegisterAllocator *alc, MVMJitTileList *list, MVMint32 tile_idx) {
}
                                 
static void linear_scan(MVMThreadContext *tc, RegisterAllocator *alc, MVMJitTileList *list) {
    MVMint32 i, tile_cursor = 0;
    MVM_VECTOR_INIT(alc->retired, alc->worklist_num);
    MVM_VECTOR_INIT(alc->spilled, 8);
    _DEBUG("STARTING LINEAR SCAN\n\n");
    while (alc->worklist_num > 0) {
        MVMint32 v        = live_range_heap_pop(alc->values, alc->worklist, &alc->worklist_num);
        MVMint32 order_nr = first_ref(alc->values + v);
        MVMint32 first_pos = order_nr / 2; /* round down */
        MVMint8 reg;
        _DEBUG("Processing live range %d (first ref %d, last ref %d)\n", v, first_ref(alc->values + v), last_ref(alc->values + v));
        /* NB: Should I have a compaction step to remove these? */
        if (live_range_is_empty(alc->values + v))
            continue;

        /* deal with 'special' requirements */
        for (; tile_cursor <= first_pos; tile_cursor++) {
            MVMJitTile *tile = list->items[tile_cursor];
            if (tile->op == MVM_JIT_ARGLIST) {
                compile_arglist_node(tc, alc, list, tile_cursor);
            } else if (tile->op == MVM_JIT_CALL) {
                spill_over_call_node(tc, alc, list, tile_cursor);
            } else {
                /* deal with 'use' registers */
                for  (i = 1; i < tile->num_refs; i++) {
                    MVMint8 spec = MVM_JIT_REGISTER_FETCH(tile->register_spec, i);
                    if (MVM_JIT_REGISTER_IS_USED(spec) &&
                        MVM_JIT_REGISTER_HAS_REQUIREMENT(spec)) {
                        NYI(tile_use_requirements);
                    }
                }
            }
        }

        /* assign registers in loop */
        active_set_expire(tc, alc, order_nr);
        if (MVM_JIT_REGISTER_HAS_REQUIREMENT(alc->values[v].register_spec)) {
            reg = MVM_JIT_REGISTER_REQUIREMENT(alc->values[v].register_spec);
            if (NVR_GPR_BITMAP & (1 << reg)) {
                assign_register(tc, alc, list, v, MVM_JIT_STORAGE_NVR, reg);
            } else {
                /* TODO; might require swapping / spilling */
                NYI(general_purpose_register_spec);
            }
        } else {
            while ((reg = get_register(tc, alc, MVM_JIT_STORAGE_GPR)) < 0) {
                spill_any_register(tc, alc, list, order_nr);
            }
            assign_register(tc, alc, list, v, MVM_JIT_STORAGE_GPR, reg);
            active_set_add(tc, alc, v);
        }
    }
    /* flush active live ranges */
    active_set_expire(tc, alc, list->items_num + 1);
    _DEBUG("END OF LINEAR SCAN\n\n");
}


void MVM_jit_linear_scan_allocate(MVMThreadContext *tc, MVMJitCompiler *compiler, MVMJitTileList *list) {
    RegisterAllocator alc;
    /* initialize allocator */
    alc.compiler = compiler;
    /* restart spill stack */

    alc.active_top = 0;
    memset(alc.active, -1, sizeof(alc.active));

    alc.reg_give = alc.reg_take = 0;
    memcpy(alc.reg_ring, available_gpr,
           sizeof(available_gpr));

    /* run algorithm */
    determine_live_ranges(tc, &alc, list);
    linear_scan(tc, &alc, list);

    /* deinitialize allocator */
    MVM_free(alc.sets);
    MVM_free(alc.refs);
    MVM_free(alc.values);

    MVM_free(alc.worklist);
    MVM_free(alc.retired);
    MVM_free(alc.spilled);


    /* make edits effective */
    MVM_jit_tile_list_edit(tc, list);

}