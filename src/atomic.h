/*
atomic.h - Atomic operations
Copyright (C) 2021  cerg2010cerg2010 <github.com/cerg2010cerg2010>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#ifndef ATOMIC_H
#define ATOMIC_H

#if __STDC_VERSION__ >= 201112LL
#ifndef __STDC_NO_ATOMICS__
#include <stdatomic.h>
#define C11_ATOMICS
#endif
#endif

#ifndef C11_ATOMICS

typedef int atomic_int;

#ifdef __GNUC__
enum
{
	memory_order_relaxed = __ATOMIC_RELAXED,
	memory_order_acquire = __ATOMIC_ACQUIRE,
	memory_order_release = __ATOMIC_RELEASE,
	memory_order_acq_rel = __ATOMIC_ACQ_REL,
	memory_order_seq_cst = __ATOMIC_SEQ_CST,
};

#define atomic_exchange(A, C) __atomic_exchange_n(A, C, __ATOMIC_SEQ_CST)
#define atomic_compare_exchange_weak(A, E, D) __atomic_compare_exchange_n(A, E, D, 1, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_compare_exchange_strong(A, E, D) __atomic_compare_exchange_n(A, E, D, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_store(A, C) __atomic_store_n(A, C, __ATOMIC_SEQ_CST)
#define atomic_load(A) __atomic_load_n(A, __ATOMIC_SEQ_CST)

#define atomic_exchange_explicit(A, C, O) __atomic_exchange_n(A, C, O)
#define atomic_compare_exchange_weak_explicit(A, E, D, S, F) __atomic_compare_exchange_n(A, E, D, 1, S, F)
#define atomic_compare_exchange_strong_explicit(A, E, D, S, F) __atomic_compare_exchange_n(A, E, D, 0, S, F)
#define atomic_store_explicit(A, C, O) __atomic_store_n(A, C, O)
#define atomic_load_explicit(A, O) __atomic_load_n(A, O)

#define atomic_thread_fence(O) __atomic_thread_fence(O)
#else
#warning No atomics support for current build target!
enum
{
	memory_order_relaxed,
	memory_order_acquire,
	memory_order_release,
	memory_order_acq_rel,
	memory_order_seq_cst,
};

#define atomic_store(A, C) (*(A) = (C))

#define atomic_exchange(A, C) ((*(A) + (C)) - (*(A) = (C)))
#define atomic_compare_exchange_weak(A, E, D) ((*(A) == *(E)) ? (*(A) = (D)), true : (*(E) = *(A)), false)
#define atomic_compare_exchange_strong(A, E, D) atomic_compare_exchange_weak(A, E, D)
#define atomic_store(A, C) (*(A) = (C))
#define atomic_load(A) *(A)

#define atomic_exchange_explicit(A, C, O) atomic_exchange(A, C)
#define atomic_compare_exchange_weak_explicit(A, E, D, S, F) atomic_compare_exchange_weak(A, E, D)
#define atomic_compare_exchange_strong_explicit(A, E, D, S, F) atomic_compare_exchange_strong(A, E, D)
#define atomic_store_explicit(A, C, O) atomic_store(A, C)
#define atomic_load_explicit(A, O) atomic_load(A)

#define atomic_thread_fence(O)
#endif

#endif /* C11_ATOMICS */

#endif
