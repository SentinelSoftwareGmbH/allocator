#include "allocator.h"
#include <stdbool.h>   /* bool, true, false       */
#include <stdint.h>    /* uintptr_t, uint_fast8_t */
#include <stdalign.h>  /* max_align_t, alignof    */
#include <string.h>    /* memcpy() */

/* A FreeNode header prefixes blocks in the freelist,
 * putting them in a circular singly-linked list and
 * storing size information.
 */
typedef struct FreeNode {
	/* block size in terms of UNITSZ */
	size_t nunits;
	struct FreeNode *nxt;
	/* aligning header to strictest type also aligns blocks */ 
	size_t _align[];
} FreeNode_t;

enum {UNITSZ = sizeof(FreeNode_t)};

/* Returns least value to add to base to align it to aln */
static inline uint_fast8_t aln_offset(uintptr_t base, uint_fast8_t aln)
{
	return base%aln? aln - base%aln : 0;
}
 
#if ATOMIC_BOOL_LOCK_FREE == 2
/* Test and test-and-set */
static inline void spinlock(atomic_bool *lock)
{
	retry :
	if (atomic_exchange_explicit(lock, true, memory_order_acquire)) {
		/* As it loops on a load, this performs better 
		 * on many processors where atomic loads are cheaper
		 * than atomic exchanges.
		 * This is why atomic_bool is preferred for the lock if
		 * it is lock-free, as atomic_flag cannot be loaded.
		 */
		while (atomic_load_explicit(lock, memory_order_acquire))
			;
		goto retry;
	}
}
static inline void spinunlock(atomic_bool *lock)
{
	atomic_store_explicit(lock, false, memory_order_release);
}
#else
/* Test-and-set */
static inline void spinlock(atomic_flag *lock)
{
	while (atomic_flag_test_and_set_explicit(lock, memory_order_acquire))
		;
}
static inline void spinunlock(atomic_flag *lock)
{
	atomic_flag_clear_explicit(lock, memory_order_release);
}
#endif

/* K&R style next fit allocator */
void *allocator_alloc(allocator_t *a, size_t nbytes)
{
	/* If 0 size allocation is requested
	 * or rounding up the requested size would overflow,
	 * or allocator's freelist pointer is NULL (no freelist)
	 * return NULL.
	 */

	uint_fast8_t inc = aln_offset(nbytes, UNITSZ);
	if (!nbytes || SIZE_MAX-inc < nbytes)
		return NULL;
	
	void *res = NULL;
	spinlock(&a->lock);
	if (a->p) {
		/* Round up nbytes to next number of units, +1 unit for header */
		size_t nunits = (nbytes+inc)/UNITSZ + 1;

		for (FreeNode_t *prv = a->p, *cur = prv->nxt ;; prv = cur, cur = cur->nxt) {
			if (cur->nunits >= nunits) {         /* match found ! */
				if (cur->nunits == nunits) { /* unlink block  */
					if (prv->nxt != cur->nxt)
						prv->nxt = cur->nxt;
					else /* freelist is singleton */
						prv = NULL; /* NULL signifies no freelist */
				} else /* adjust size & allocate from tail */
					(cur += (cur->nunits -= nunits))->nunits = nunits;
				a->p = prv;   /* Aids freelist consistency  */
				res  = cur+1; /* Usable region after header */
				break;
			} else if (cur == a->p) /* wrapped around, no matches */
				break;
		}
	}
	spinunlock(&a->lock);
	return res;
}

/* Add ptr to a's freelist */
void allocator_free(allocator_t *a, void *restrict ptr)
{
	FreeNode_t *p = ptr;
	if (p--) { /* No-op if p is NULL */
		spinlock(&a->lock);
		if (a->p) {
			/* Freelist is in ascending order of addresses,
			 * traverse to reach insertion point.
			 */
			FreeNode_t *cur;
			for (cur = a->p; !(p > cur && p < cur->nxt); cur = cur->nxt) {
				if(cur >= cur->nxt && (p > cur || p < cur->nxt))
					break;
			}

			if (p + p->nunits == cur->nxt) { /* Coalesce  with nxt */
				p->nunits += cur->nxt->nunits;
				p->nxt = cur->nxt->nxt;
			} else /* Insert p after cur */
				p->nxt = cur->nxt;
			if (cur + cur->nunits == p) {   /* Coalesce with prv   */
				cur->nunits += p->nunits;
				cur->nxt = p->nxt;
			} else /* Insert p after cur */
				cur->nxt = p;
			a->p = cur;
		} else /* If no freelist, create singleton with p */
			a->p = p, p->nxt = p;
		spinunlock(&a->lock);
	}
}

void allocator_add(allocator_t *a, void *restrict p, size_t nbytes)
{
	uintptr_t addr = (uintptr_t)p;
	uint_fast8_t inc = aln_offset(addr, alignof(size_t));
	size_t nunits = (nbytes - inc)/UNITSZ; /* Round down */

	/* Ensure aligned pointer doesn't exceed bounds,
	 * and given size is of at least one unit.
	 */
	if (nbytes > inc+UNITSZ && nunits) {
		FreeNode_t *new = (FreeNode_t *)(addr+inc); /* Create header */
		new->nunits = nunits;
		allocator_free(a, new+1);
	}
}

size_t allocator_allocsz(const void *restrict p)
{
	return p? ( ((FreeNode_t *)p-1)->nunits-1 ) * UNITSZ : 0;	
}

void allocator_for_blocks(allocator_t *a, void(*f)(size_t))
{
	spinlock(&a->lock);
	if (a->p) {
		FreeNode_t *cur = a->p;
		do {
			f(allocator_allocsz(cur+1));
			cur = cur->nxt;

		} while (cur != a->p);
	}
	spinunlock(&a->lock);
}

void *allocator_realloc(allocator_t *a, void *restrict p, size_t nbytes)
{
	if (!p)
		return allocator_alloc(a, nbytes);
	else if (!nbytes) {
		allocator_free(a, p);
		return NULL;
	} else {
		void *res = p;
		if (
			allocator_allocsz(p) < nbytes 
			&& (res = allocator_alloc(a, nbytes))
		)
			memcpy(res, p, allocator_allocsz(p)), allocator_free(a, p);
		return res;
	}
}
