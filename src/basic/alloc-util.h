/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <alloca.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "macro.h"

#if HAS_FEATURE_MEMORY_SANITIZER
#  include <sanitizer/msan_interface.h>
#endif

typedef void (*free_func_t)(void *p);

/* If for some reason more than 4M are allocated on the stack, let's abort immediately. It's better than
 * proceeding and smashing the stack limits. Note that by default RLIMIT_STACK is 8M on Linux. */
#define ALLOCA_MAX (4U*1024U*1024U)

#define new(t, n) ((t*) malloc_multiply(sizeof(t), (n)))

#define new0(t, n) ((t*) calloc((n) ?: 1, sizeof(t)))

#define newa(t, n)                                                      \
        ({                                                              \
                size_t _n_ = n;                                         \
                assert(!size_multiply_overflow(sizeof(t), _n_));        \
                assert(sizeof(t)*_n_ <= ALLOCA_MAX);                    \
                (t*) alloca((sizeof(t)*_n_) ?: 1);                      \
        })

#define newa0(t, n)                                                     \
        ({                                                              \
                size_t _n_ = n;                                         \
                assert(!size_multiply_overflow(sizeof(t), _n_));        \
                assert(sizeof(t)*_n_ <= ALLOCA_MAX);                    \
                (t*) alloca0((sizeof(t)*_n_) ?: 1);                     \
        })

#define newdup(t, p, n) ((t*) memdup_multiply(p, sizeof(t), (n)))

#define newdup_suffix0(t, p, n) ((t*) memdup_suffix0_multiply(p, sizeof(t), (n)))

#define malloc0(n) (calloc(1, (n) ?: 1))

static inline void *mfree(void *memory) {
        free(memory);
        return NULL;
}

#define free_and_replace(a, b)                  \
        ({                                      \
                free(a);                        \
                (a) = (b);                      \
                (b) = NULL;                     \
                0;                              \
        })

void* memdup(const void *p, size_t l) _alloc_(2);
void* memdup_suffix0(const void *p, size_t l); /* We can't use _alloc_() here, since we return a buffer one byte larger than the specified size */

#define memdupa(p, l)                           \
        ({                                      \
                void *_q_;                      \
                size_t _l_ = l;                 \
                assert(_l_ <= ALLOCA_MAX);      \
                _q_ = alloca(_l_ ?: 1);         \
                memcpy(_q_, p, _l_);            \
        })

#define memdupa_suffix0(p, l)                   \
        ({                                      \
                void *_q_;                      \
                size_t _l_ = l;                 \
                assert(_l_ <= ALLOCA_MAX);      \
                _q_ = alloca(_l_ + 1);          \
                ((uint8_t*) _q_)[_l_] = 0;      \
                memcpy(_q_, p, _l_);            \
        })

static inline void freep(void *p) {
        *(void**)p = mfree(*(void**) p);
}

#define _cleanup_free_ _cleanup_(freep)

static inline bool size_multiply_overflow(size_t size, size_t need) {
        return _unlikely_(need != 0 && size > (SIZE_MAX / need));
}

_malloc_  _alloc_(1, 2) static inline void *malloc_multiply(size_t size, size_t need) {
        if (size_multiply_overflow(size, need))
                return NULL;

        return malloc(size * need ?: 1);
}

#if !HAVE_REALLOCARRAY
_alloc_(2, 3) static inline void *reallocarray(void *p, size_t need, size_t size) {
        if (size_multiply_overflow(size, need))
                return NULL;

        return realloc(p, size * need ?: 1);
}
#endif

_alloc_(2, 3) static inline void *memdup_multiply(const void *p, size_t size, size_t need) {
        if (size_multiply_overflow(size, need))
                return NULL;

        return memdup(p, size * need);
}

/* Note that we can't decorate this function with _alloc_() since the returned memory area is one byte larger
 * than the product of its parameters. */
static inline void *memdup_suffix0_multiply(const void *p, size_t size, size_t need) {
        if (size_multiply_overflow(size, need))
                return NULL;

        return memdup_suffix0(p, size * need);
}

void* greedy_realloc(void **p, size_t *allocated, size_t need, size_t size);
void* greedy_realloc0(void **p, size_t *allocated, size_t need, size_t size);

#define GREEDY_REALLOC(array, allocated, need)                          \
        greedy_realloc((void**) &(array), &(allocated), (need), sizeof((array)[0]))

#define GREEDY_REALLOC0(array, allocated, need)                         \
        greedy_realloc0((void**) &(array), &(allocated), (need), sizeof((array)[0]))

#define alloca0(n)                                      \
        ({                                              \
                char *_new_;                            \
                size_t _len_ = n;                       \
                assert(_len_ <= ALLOCA_MAX);            \
                _new_ = alloca(_len_ ?: 1);             \
                (void *) memset(_new_, 0, _len_);       \
        })

/* It's not clear what alignment glibc/gcc alloca() guarantee, hence provide a guaranteed safe version */
#define alloca_align(size, align)                                       \
        ({                                                              \
                void *_ptr_;                                            \
                size_t _mask_ = (align) - 1;                            \
                size_t _size_ = size;                                   \
                assert(_size_ <= ALLOCA_MAX);                           \
                _ptr_ = alloca((_size_ + _mask_) ?: 1);                 \
                (void*)(((uintptr_t)_ptr_ + _mask_) & ~_mask_);         \
        })

#define alloca0_align(size, align)                                      \
        ({                                                              \
                void *_new_;                                            \
                size_t _xsize_ = (size);                                \
                _new_ = alloca_align(_xsize_, (align));                 \
                (void*)memset(_new_, 0, _xsize_);                       \
        })

#if HAS_FEATURE_MEMORY_SANITIZER
#  define msan_unpoison(r, s) __msan_unpoison(r, s)
#else
#  define msan_unpoison(r, s)
#endif

/* This returns the number of usable bytes in a malloc()ed region as per malloc_usable_size(), in a way that
 * is compatible with _FORTIFY_SOURCES. If _FORTIFY_SOURCES is used many memory operations will take the
 * object size as returned by __builtin_object_size() into account. Hence, let's return the smaller size of
 * malloc_usable_size() and __builtin_object_size() here, so that we definitely operate in safe territory by
 * both the compiler's and libc's standards. Note that __builtin_object_size() evaluates to SIZE_MAX if the
 * size cannot be determined, hence the MIN() expression should be safe with dynamically sized memory,
 * too. Moreover, when NULL is passed malloc_usable_size() is documented to return zero, and
 * __builtin_object_size() returns SIZE_MAX too, hence we also return a sensible value of 0 in this corner
 * case. */
#define MALLOC_SIZEOF_SAFE(x) \
        MIN(malloc_usable_size(x), __builtin_object_size(x, 0))

/* Inspired by ELEMENTSOF() but operates on malloc()'ed memory areas: typesafely returns the number of items
 * that fit into the specified memory block */
#define MALLOC_ELEMENTSOF(x) \
        (__builtin_choose_expr(                                         \
                __builtin_types_compatible_p(typeof(x), typeof(&*(x))), \
                MALLOC_SIZEOF_SAFE(x)/sizeof((x)[0]),                   \
                VOID_0))
