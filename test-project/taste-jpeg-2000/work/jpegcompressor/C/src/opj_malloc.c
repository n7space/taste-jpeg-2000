/*
 * The copyright in this software is being made available under the 2-clauses
 * BSD License, included below. This software may be subject to other third
 * party and contributor rights, including patent rights, and no such rights
 * are granted under this license.
 *
 * Copyright (c) 2015, Mathieu Malaterre <mathieu.malaterre@gmail.com>
 * Copyright (c) 2015, Matthieu Darbois
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS `AS IS'
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#define OPJ_SKIP_POISON
#include "opj_includes.h"

#ifndef SIZE_MAX
# define SIZE_MAX ((size_t) -1)
#endif

/*
 * Static memory pool allocator.
 *
 * No dynamic memory allocation is performed: every allocation of the library
 * is served from a single buffer provided by the user with
 * opj_mem_pool_init() (typically a statically allocated array).
 *
 * The pool is split into blocks, each starting with a header holding its own
 * size and the size of the physically preceding block, so neighbouring free
 * blocks can be merged in constant time. Free blocks are kept in an address
 * ordered, doubly linked list (links stored in the free block payload) and
 * allocation is first fit. A zero sized "in use" sentinel block terminates
 * the pool.
 *
 * Every returned pointer is aligned to OPJ_MEM_ALIGN bytes; larger alignments
 * are obtained by splitting off a free block in front of the allocation.
 *
 * The pool is not thread-safe.
 */

/* Alignment of every block and of every returned pointer */
#define OPJ_MEM_ALIGN 16U

#define OPJ_MEM_ROUND_UP(x, a) (((x) + ((a) - 1U)) & ~((size_t)(a) - 1U))

/* "In use" flag, stored in bit 0 of opj_mem_block_t::size */
#define OPJ_MEM_USED ((size_t)1U)

/* Header in front of every block, used or free */
typedef struct opj_mem_block {
    /* size of the whole block in bytes, header included, ORed with OPJ_MEM_USED */
    size_t size;
    /* size of the physically preceding block, 0 for the first block */
    size_t prev_size;
} opj_mem_block_t;

/* Free list links, stored in the payload of free blocks */
typedef struct opj_mem_links {
    opj_mem_block_t *prev;
    opj_mem_block_t *next;
} opj_mem_links_t;

#define OPJ_MEM_HDR_SIZE \
    OPJ_MEM_ROUND_UP(sizeof(opj_mem_block_t), OPJ_MEM_ALIGN)
#define OPJ_MEM_MIN_BLOCK \
    OPJ_MEM_ROUND_UP(OPJ_MEM_HDR_SIZE + sizeof(opj_mem_links_t), OPJ_MEM_ALIGN)

static opj_mem_block_t *opj_mem_free_list = NULL;
static OPJ_BOOL opj_mem_initialized = OPJ_FALSE;
static size_t opj_mem_used = 0U;
static size_t opj_mem_peak = 0U;

static INLINE size_t opj_mem_size(const opj_mem_block_t *b)
{
    return b->size & ~OPJ_MEM_USED;
}

static INLINE OPJ_BOOL opj_mem_is_used(const opj_mem_block_t *b)
{
    return (b->size & OPJ_MEM_USED) != 0U;
}

static INLINE opj_mem_links_t *opj_mem_links(opj_mem_block_t *b)
{
    return (opj_mem_links_t *)(void *)((OPJ_UINT8 *)b + OPJ_MEM_HDR_SIZE);
}

static INLINE opj_mem_block_t *opj_mem_next(opj_mem_block_t *b)
{
    return (opj_mem_block_t *)(void *)((OPJ_UINT8 *)b + opj_mem_size(b));
}

static INLINE opj_mem_block_t *opj_mem_prev(opj_mem_block_t *b)
{
    if (b->prev_size == 0U) {
        return NULL;
    }
    return (opj_mem_block_t *)(void *)((OPJ_UINT8 *)b - b->prev_size);
}

static INLINE void *opj_mem_payload(opj_mem_block_t *b)
{
    return (OPJ_UINT8 *)b + OPJ_MEM_HDR_SIZE;
}

static INLINE opj_mem_block_t *opj_mem_block_of(void *ptr)
{
    return (opj_mem_block_t *)(void *)((OPJ_UINT8 *)ptr - OPJ_MEM_HDR_SIZE);
}

/* Set the size and state of a block, keeping the next block's back link */
static INLINE void opj_mem_set(opj_mem_block_t *b, size_t size, size_t used)
{
    b->size = size | used;
    opj_mem_next(b)->prev_size = size;
}

static INLINE void opj_mem_account(size_t freed, size_t allocated)
{
    opj_mem_used = opj_mem_used - freed + allocated;
    if (opj_mem_used > opj_mem_peak) {
        opj_mem_peak = opj_mem_used;
    }
}

static void opj_mem_list_insert(opj_mem_block_t *b)
{
    opj_mem_block_t *prev = NULL;
    opj_mem_block_t *next = opj_mem_free_list;

    while ((next != NULL) && (next < b)) {
        prev = next;
        next = opj_mem_links(next)->next;
    }
    opj_mem_links(b)->prev = prev;
    opj_mem_links(b)->next = next;
    if (prev != NULL) {
        opj_mem_links(prev)->next = b;
    } else {
        opj_mem_free_list = b;
    }
    if (next != NULL) {
        opj_mem_links(next)->prev = b;
    }
}

static void opj_mem_list_remove(opj_mem_block_t *b)
{
    opj_mem_block_t *prev = opj_mem_links(b)->prev;
    opj_mem_block_t *next = opj_mem_links(b)->next;

    if (prev != NULL) {
        opj_mem_links(prev)->next = next;
    } else {
        opj_mem_free_list = next;
    }
    if (next != NULL) {
        opj_mem_links(next)->prev = prev;
    }
}

/* Turn a block that is not in the free list into a free block, merging it
 * with its free neighbours */
static void opj_mem_release(opj_mem_block_t *b)
{
    size_t size = opj_mem_size(b);
    opj_mem_block_t *next = opj_mem_next(b);
    opj_mem_block_t *prev = opj_mem_prev(b);

    if (!opj_mem_is_used(next)) {
        opj_mem_list_remove(next);
        size += opj_mem_size(next);
    }
    if ((prev != NULL) && !opj_mem_is_used(prev)) {
        /* prev already is in the free list, just grow it */
        opj_mem_set(prev, opj_mem_size(prev) + size, 0U);
    } else {
        opj_mem_set(b, size, 0U);
        opj_mem_list_insert(b);
    }
}

/* Reduce a used block to block_size bytes, releasing the tail if large
 * enough to form a block */
static void opj_mem_shrink(opj_mem_block_t *b, size_t block_size)
{
    size_t size = opj_mem_size(b);

    if (size - block_size >= OPJ_MEM_MIN_BLOCK) {
        opj_mem_block_t *rest;

        opj_mem_set(b, block_size, OPJ_MEM_USED);
        rest = opj_mem_next(b);
        opj_mem_set(rest, size - block_size, OPJ_MEM_USED);
        opj_mem_release(rest);
    }
}

/* Block size needed to hold size bytes of payload, 0 on overflow */
static INLINE size_t opj_mem_block_size(size_t size)
{
    size_t block_size;

    if (size > SIZE_MAX - OPJ_MEM_HDR_SIZE - OPJ_MEM_ALIGN) {
        return 0U;
    }
    block_size = OPJ_MEM_ROUND_UP(size + OPJ_MEM_HDR_SIZE, OPJ_MEM_ALIGN);
    if (block_size < OPJ_MEM_MIN_BLOCK) {
        block_size = OPJ_MEM_MIN_BLOCK;
    }
    return block_size;
}

static void *opj_mem_alloc(size_t alignment, size_t size)
{
    opj_mem_block_t *b;
    size_t block_size;
    size_t gap = 0U;

    /* alignment shall be power of 2 */
    assert((alignment != 0U) && ((alignment & (alignment - 1U)) == 0U));

    if ((size == 0U) || !opj_mem_initialized) {
        return NULL;
    }
    block_size = opj_mem_block_size(size);
    if (block_size == 0U) {
        return NULL;
    }

    /* first fit */
    for (b = opj_mem_free_list; b != NULL; b = opj_mem_links(b)->next) {
        size_t b_size = opj_mem_size(b);

        if (alignment > OPJ_MEM_ALIGN) {
            /* we simply use `size_t` to cast, since we are only interest in
             * binary AND operator */
            gap = (alignment - ((size_t)opj_mem_payload(b) & (alignment - 1U))) &
                  (alignment - 1U);
            /* the gap in front becomes a free block, so it must be big enough */
            while ((gap != 0U) && (gap < OPJ_MEM_MIN_BLOCK)) {
                gap += alignment;
            }
        }
        if ((b_size >= gap) && (b_size - gap >= block_size)) {
            break;
        }
    }
    if (b == NULL) {
        return NULL;
    }

    if (gap != 0U) {
        /* keep the gap as a free block, it stays in place in the free list */
        size_t b_size = opj_mem_size(b);

        opj_mem_set(b, gap, 0U);
        b = opj_mem_next(b);
        opj_mem_set(b, b_size - gap, OPJ_MEM_USED);
    } else {
        opj_mem_list_remove(b);
        opj_mem_set(b, opj_mem_size(b), OPJ_MEM_USED);
    }
    opj_mem_shrink(b, block_size);
    opj_mem_account(0U, opj_mem_size(b));

    return opj_mem_payload(b);
}

static void *opj_mem_realloc(void *ptr, size_t alignment, size_t new_size)
{
    opj_mem_block_t *b;
    opj_mem_block_t *next;
    size_t block_size;
    size_t old_size;
    void *new_ptr;

    if (new_size == 0U) { /* prevent implementation defined behavior of realloc */
        return NULL;
    }
    if (ptr == NULL) {
        return opj_mem_alloc(alignment, new_size);
    }
    block_size = opj_mem_block_size(new_size);
    if (block_size == 0U) {
        return NULL;
    }

    b = opj_mem_block_of(ptr);
    assert(opj_mem_is_used(b));
    old_size = opj_mem_size(b);

    /* resize in place (keeps alignment): shrink, or grow into next free block */
    next = opj_mem_next(b);
    if (block_size <= old_size) {
        opj_mem_shrink(b, block_size);
        opj_mem_account(old_size, opj_mem_size(b));
        return ptr;
    }
    if (!opj_mem_is_used(next) &&
            (old_size + opj_mem_size(next) >= block_size)) {
        opj_mem_list_remove(next);
        opj_mem_set(b, old_size + opj_mem_size(next), OPJ_MEM_USED);
        opj_mem_shrink(b, block_size);
        opj_mem_account(old_size, opj_mem_size(b));
        return ptr;
    }

    /* move; on failure the original block is left untouched */
    new_ptr = opj_mem_alloc(alignment, new_size);
    if (new_ptr == NULL) {
        return NULL;
    }
    memcpy(new_ptr, ptr, old_size - OPJ_MEM_HDR_SIZE);
    opj_free(ptr);
    return new_ptr;
}

OPJ_BOOL OPJ_CALLCONV opj_mem_pool_init(void *buffer, OPJ_SIZE_T size)
{
    OPJ_UINT8 *start;
    size_t misalignment;
    opj_mem_block_t *first;
    opj_mem_block_t *sentinel;

    opj_mem_initialized = OPJ_FALSE;
    opj_mem_free_list = NULL;
    opj_mem_used = 0U;
    opj_mem_peak = 0U;

    if (buffer == NULL) {
        return OPJ_FALSE;
    }
    misalignment = (OPJ_MEM_ALIGN - ((size_t)buffer & (OPJ_MEM_ALIGN - 1U))) &
                   (OPJ_MEM_ALIGN - 1U);
    if (size < misalignment + OPJ_MEM_MIN_BLOCK + OPJ_MEM_HDR_SIZE) {
        return OPJ_FALSE;
    }
    start = (OPJ_UINT8 *)buffer + misalignment;
    size = (size - misalignment) & ~((size_t)OPJ_MEM_ALIGN - 1U);

    first = (opj_mem_block_t *)(void *)start;
    first->prev_size = 0U;
    opj_mem_set(first, size - OPJ_MEM_HDR_SIZE, 0U);
    sentinel = opj_mem_next(first);
    sentinel->size = OPJ_MEM_USED;

    opj_mem_links(first)->prev = NULL;
    opj_mem_links(first)->next = NULL;
    opj_mem_free_list = first;
    opj_mem_initialized = OPJ_TRUE;

    return OPJ_TRUE;
}

OPJ_SIZE_T OPJ_CALLCONV opj_mem_pool_get_used(void)
{
    return opj_mem_used;
}

OPJ_SIZE_T OPJ_CALLCONV opj_mem_pool_get_peak(void)
{
    return opj_mem_peak;
}

void * opj_malloc(size_t size)
{
    return opj_mem_alloc(OPJ_MEM_ALIGN, size);
}
void * opj_calloc(size_t num, size_t size)
{
    void *ptr;

    if (num == 0 || size == 0) {
        /* prevent implementation defined behavior of realloc */
        return NULL;
    }
    if (num > SIZE_MAX / size) {
        return NULL;
    }
    ptr = opj_mem_alloc(OPJ_MEM_ALIGN, num * size);
    if (ptr != NULL) {
        memset(ptr, 0, num * size);
    }
    return ptr;
}

void *opj_aligned_malloc(size_t size)
{
    return opj_mem_alloc(16U, size);
}
void * opj_aligned_realloc(void *ptr, size_t size)
{
    return opj_mem_realloc(ptr, 16U, size);
}

void *opj_aligned_32_malloc(size_t size)
{
    return opj_mem_alloc(32U, size);
}
void * opj_aligned_32_realloc(void *ptr, size_t size)
{
    return opj_mem_realloc(ptr, 32U, size);
}

void opj_aligned_free(void* ptr)
{
    opj_free(ptr);
}

void * opj_realloc(void *ptr, size_t new_size)
{
    return opj_mem_realloc(ptr, OPJ_MEM_ALIGN, new_size);
}
void opj_free(void *ptr)
{
    opj_mem_block_t *b;

    if (ptr == NULL) {
        return;
    }
    b = opj_mem_block_of(ptr);
    assert(opj_mem_is_used(b));
    opj_mem_account(opj_mem_size(b), 0U);
    opj_mem_release(b);
}
