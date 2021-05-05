/*
spinlock.h - Atomic spinlock
Copyright (C) 2021  LekKit <github.com/LekKit>

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

#ifndef RISCV_SPINLOCK_H
#define RISCV_SPINLOCK_H

#include <stdatomic.h>
#include <stdint.h>
#include "threading.h"

typedef struct {
    atomic_int flag;
} spinlock_t;

static inline void spin_init(spinlock_t* lock)
{
    atomic_store_explicit(&lock->flag, 0, memory_order_relaxed);
}

static inline void spin_lock(spinlock_t* lock)
{
    while (atomic_exchange_explicit(&lock->flag, 1, memory_order_acquire));
}

static inline void spin_unlock(spinlock_t* lock)
{
    atomic_store_explicit(&lock->flag, 0, memory_order_release);
}

#endif
