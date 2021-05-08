/*
riscv32.c - RISC-V Virtual Machine
Copyright (C) 2021  LekKit <github.com/LekKit>
                    Mr0maks <mr.maks0443@gmail.com>

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

#include <stdarg.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "riscv.h"
#include "riscv32.h"
#include "cpu/riscv_cpu.h"
#include "riscv32i_registers.h"
#include "riscv32_mmu.h"
#include "riscv32_csr.h"
#include "mem_ops.h"
#include "threading.h"
#include "spinlock.h"

#include "devices/ns16550a.h"
#include "devices/clint.h"
#include "devices/fb_window.h"
#include "devices/plic.h"
#include "devices/ps2-altera.h"
#include "devices/ps2-mouse.h"
#include "devices/ps2-keyboard.h"

// This should redirect the VM to the trap handlers when they are implemented
void riscv32c_illegal_insn(riscv32_vm_state_t *vm, const uint16_t instruction)
{
    riscv32_debug_always(vm, "RV32C: illegal instruction %h", instruction);
    riscv32_trap(vm, TRAP_ILL_INSTR, instruction);
}

void riscv32_illegal_insn(riscv32_vm_state_t *vm, const uint32_t instruction)
{
    riscv32_debug_always(vm, "RV32I: illegal instruction %h", instruction);
    riscv32_trap(vm, TRAP_ILL_INSTR, instruction);
}

#define MAX_VMS 256
#define MAX_SCREENS 2

/*static*/ spinlock_t global_lock;
static riscv32_vm_state_t* global_vm_list[MAX_VMS] = {NULL};
static size_t global_vm_count = 0;
static thread_handle_t global_irq_thread;
static struct fb_data global_screens[MAX_SCREENS];
static size_t global_screen_count = 0;

static void* global_irq_handler(void* arg)
{
    riscv32_vm_state_t* vm;
    while (true) {
        sleep_ms(10);
        spin_lock(&global_lock);
        for (size_t i=0, vmcnt = 0; i<MAX_VMS; ++i) {
            vm = global_vm_list[i];
            if (vm == NULL) {
                    continue;
            }

            /*
            * Queue interrupt data & flag, wake CPU thread.
            * Technically, writing to wait_event is a race condition,
            * but this doesn't matter - failing to deliver an event will
            * simply delay it, and sending a spurious event merely lowers performance
            */
            vm->ev_int_mask |= (1 << INTERRUPT_MTIMER);
            vm->ev_int = true;
            atomic_store_explicit(&vm->wait_event, 0, memory_order_release);

            if (++vmcnt == global_vm_count) {
                break;
            }
        }
        spin_unlock(&global_lock);
        fb_update(global_screens, global_screen_count);
    }
    return arg;
}

void riscv32_interrupt(riscv32_vm_state_t *vm, uint32_t cause)
{
    vm->ev_int_mask |= (1 << cause);
    vm->ev_int = true;
    atomic_store_explicit(&vm->wait_event, 0, memory_order_release);
}

static bool register_vm(riscv32_vm_state_t *vm, uint32_t hartid)
{
    if (hartid >= MAX_VMS) {
        return false;
    }

    spin_lock(&global_lock);
    global_vm_list[hartid] = vm;
    vm->csr.hartid = hartid;
    global_vm_count++;
    spin_unlock(&global_lock);
    return true;
}

static void deregister_vm(riscv32_vm_state_t *vm)
{
    spin_lock(&global_lock);
    global_vm_list[vm->csr.hartid] = NULL;
    if (global_vm_count == 0) {
        thread_kill(global_irq_thread);
    }
    spin_unlock(&global_lock);
}

static bool devices_init(riscv32_vm_state_t *vm)
{
    // 0x10000 pages = 256M
    if (!riscv32_init_phys_mem(&vm->mem, 0x80000000, 0x10000)) {
        printf("Failed to allocate VM physical RAM!\n");
        return false;
    }

    ns16550a_init(vm, 0x10000000);

    void *plic_data = plic_init(vm, 0xC000000);

    static struct ps2_device ps2_mouse;
    ps2_mouse = ps2_mouse_create();
    altps2_init(vm, 0x20000000, plic_data, 1, &ps2_mouse);

    static struct ps2_device ps2_keyboard;
    ps2_keyboard = ps2_keyboard_create();
    altps2_init(vm, 0x20001000, plic_data, 2, &ps2_keyboard);

    init_fb(vm, &global_screens[global_screen_count++], 640, 480, 0x30000000, &ps2_mouse, &ps2_keyboard);
    return true;
}

riscv32_vm_state_t *riscv32_create_vm(uint32_t hartid)
{
    if (hartid >= MAX_VMS) {
        return NULL;
    }

    riscv32_vm_state_t *vm = (riscv32_vm_state_t*)calloc(sizeof(riscv32_vm_state_t), 1);
    if (vm == NULL) {
        return NULL;
    }

    static bool global_init = false;
    if (!global_init) {
        riscv32_cpu_init();
        riscv32_priv_init();
        for (uint32_t i=0; i<4096; ++i)
            riscv32_csr_init(i, "illegal", riscv32_csr_illegal);
        riscv32_csr_m_init();
        riscv32_csr_s_init();
        riscv32_csr_u_init();
        spin_init(&global_lock);

        if (!devices_init(vm)) {
            goto err;
        }

        global_irq_thread = thread_create(global_irq_handler, NULL);
        global_init = true;
    } else {
        // Copy MMIO data from other VM so we don't need to reinit
        // all the devices again
        spin_lock(&global_lock);
        const riscv32_vm_state_t *parent_vm = NULL;
        for (size_t i = 0; i < MAX_VMS; ++i) {
            if (global_vm_list[i] != NULL) {
                parent_vm = global_vm_list[i];
                break;
            }
        }

        vm->mem = parent_vm->mem;
        vm->mmio = parent_vm->mmio;

        // Add CLINT of this hart to all other VMs so we can send an IPI
        for (size_t i = 0; i < MAX_VMS; ++i) {
            if (global_vm_list[i] == NULL) {
                continue;
            }

            riscv32_mmio_add_device(global_vm_list[i],
                    CLINT_BASE_ADDR + CLINT_LEN * hartid,
                    CLINT_BASE_ADDR + CLINT_LEN * (hartid + 1),
                    clint_mmio_handler,
                    vm);
        }
        spin_unlock(&global_lock);
    }

    riscv32_mmio_add_device(vm,
            CLINT_BASE_ADDR + CLINT_LEN * hartid,
            CLINT_BASE_ADDR + CLINT_LEN * (hartid + 1),
            clint_mmio_handler,
            vm);

    rvtimer_init(&vm->timer, 10000000); // 10 MHz timer

    riscv32_tlb_flush(vm);

    vm->mmu_virtual = false;
    vm->priv_mode = PRIVILEGE_MACHINE;
    vm->csr.edeleg[PRIVILEGE_HYPERVISOR] = 0xFFFFFFFF;
    vm->csr.ideleg[PRIVILEGE_HYPERVISOR] = 0xFFFFFFFF;
    vm->registers[REGISTER_PC] = vm->mem.begin;
    if (!register_vm(vm, hartid))
    {
        printf("Unable to register VM with id %d\n", (int)hartid);
        goto err;
    }

    return vm;
err:
    free(vm);
    return NULL;

}

void riscv32_destroy_vm(riscv32_vm_state_t *vm)
{
    deregister_vm(vm);
    for (size_t i=0; i<vm->mmio.count; ++i) {
        riscv32_mmio_remove_device(vm, vm->mmio.regions[i].base_addr);
    }
    riscv32_destroy_phys_mem(&vm->mem);
    free(vm);
}

static void riscv32_perform_interrupt(riscv32_vm_state_t *vm, uint32_t cause)
{
    uint8_t priv;
    for (priv=PRIVILEGE_MACHINE; priv>(cause & 0x3); --priv) {
        if ((vm->csr.ideleg[priv] & (1 << cause)) == 0) break;
    }
    //printf("Int %x\n", cause);
    riscv32_debug(vm, "Int %d -> %d, cause: %h, hartid: %d", vm->priv_mode, priv, cause, vm->csr.hartid);

    vm->csr.epc[priv] = riscv32i_read_register_u(vm, REGISTER_PC);
    vm->csr.cause[priv] = cause | INTERRUPT_MASK;
    vm->csr.tval[priv] = 0;
    // Save current priv mode to xPP, xIE to xPIE, disable interrupts
    if (priv == PRIVILEGE_MACHINE) {
        vm->csr.status = bit_replace(vm->csr.status, 11, 2, vm->priv_mode);
        vm->csr.status = bit_replace(vm->csr.status, 7, 1, bit_cut(vm->csr.status, 3, 1));
        vm->csr.status &= 0xFFFFFFF7;
    } else if (priv == PRIVILEGE_SUPERVISOR) {
        vm->csr.status = bit_replace(vm->csr.status, 8, 1, vm->priv_mode);
        vm->csr.status = bit_replace(vm->csr.status, 5, 1, bit_cut(vm->csr.status, 1, 1));
        vm->csr.status &= 0xFFFFFFFD;
    }
    vm->priv_mode = priv;
    atomic_store_explicit(&vm->wait_event, 0, memory_order_release);
}

void riscv32_trap(riscv32_vm_state_t *vm, uint32_t cause, uint32_t tval)
{
    uint8_t priv;
    // Delegate to lower privilege mode if needed
    for (priv=PRIVILEGE_MACHINE; priv>vm->priv_mode; --priv) {
        if ((vm->csr.edeleg[priv] & (1 << cause)) == 0) break;
    }
    riscv32_debug(vm, "Trap priv %d -> %d, cause: %h, tval: %h, hartid: %d", vm->priv_mode, priv, cause, tval, vm->csr.hartid);

    vm->csr.epc[priv] = riscv32i_read_register_u(vm, REGISTER_PC);
    vm->csr.cause[priv] = cause;
    vm->csr.tval[priv] = tval;
    // Save current priv mode to xPP, xIE to xPIE, disable interrupts
    if (priv == PRIVILEGE_MACHINE) {
        vm->csr.status = bit_replace(vm->csr.status, 11, 2, vm->priv_mode);
        vm->csr.status = bit_replace(vm->csr.status, 7, 1, bit_cut(vm->csr.status, 3, 1));
        vm->csr.status &= 0xFFFFFFF7;
    } else if (priv == PRIVILEGE_SUPERVISOR) {
        vm->csr.status = bit_replace(vm->csr.status, 8, 1, vm->priv_mode);
        vm->csr.status = bit_replace(vm->csr.status, 5, 1, bit_cut(vm->csr.status, 1, 1));
        vm->csr.status &= 0xFFFFFFFD;
    }
    vm->priv_mode = priv;
    vm->ev_trap = true;
    atomic_store_explicit(&vm->wait_event, 0, memory_order_release);
}

bool riscv32_handle_ip(riscv32_vm_state_t *vm, bool wfi)
{
    // Check if we have any pending interrupts
    if (vm->csr.ip) {
        // Loop over possible interrupt cause bits, prioritizing higher priv source
        for (uint32_t i=11; i>0; --i) {
            uint32_t imask = (1 << i);
            if (vm->csr.ip & imask) {
                uint8_t priv = i & 3;
                bool iallow = priv > vm->priv_mode;
                if (!iallow) {
                    iallow = priv == vm->priv_mode && ((vm->csr.status & (1 << priv)) || wfi);
                }
                // If individual interrupt bit is enabled & privilege allows, interrupt is executed
                //if (!(imask & vm->csr.ie)) printf("Int %x disabled!\n", i);
                if ((vm->csr.ie & imask) && iallow) {
                    // WFI should set epc to pc+4
                    if (wfi) {
                        riscv32i_write_register_u(vm, REGISTER_PC, riscv32i_read_register_u(vm, REGISTER_PC) + 4);
                        vm->ev_trap = true;
                    }
                    riscv32_perform_interrupt(vm, i);
                    return true;
                }
            }
        }
    }
    return false;
}

void riscv32_debug_func(const riscv32_vm_state_t *vm, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buffer[256];
    uint32_t size = sprintf(buffer, "[VM 0x%x] ", (uint32_t)vm->registers[REGISTER_PC]);
    uint32_t begin = 0;
    uint32_t len = strlen(fmt);
    uint32_t i;
    for (i=0; i<len; ++i) {
        if (fmt[i] == '%') {
            memcpy(buffer+size, fmt+begin, i-begin);
            size += i-begin;
            begin = i+2;
            i++;
            switch (fmt[i]) {
            case 'r':
                size += sprintf(buffer+size, "%s", riscv32i_translate_register(va_arg(ap, uint32_t)));
                break;
            case 'd':
                size += sprintf(buffer+size, "%d", va_arg(ap, int32_t));
                break;
            case 'h':
                size += sprintf(buffer+size, "0x%x", va_arg(ap, uint32_t));
                break;
            case 'c':
                size += sprintf(buffer+size, "%s", riscv32_csr_list[va_arg(ap, uint32_t)].name);
                break;
            }
        }
    }
    memcpy(buffer+size, fmt+begin, i-begin);
    size += i-begin;
    buffer[size] = 0;
    printf("%s\n", buffer);
    va_end(ap);
}

const char *riscv32i_translate_register(uint32_t reg)
{
    assert(reg < REGISTERS_MAX);
    switch (reg) {
    case REGISTER_ZERO: return "zero";
    case REGISTER_X1: return "ra";
    case REGISTER_X2: return "sp";
    case REGISTER_X3: return "gp";
    case REGISTER_X4: return "tp";
    case REGISTER_X5: return "t0";
    case REGISTER_X6: return "t1";
    case REGISTER_X7: return "t2";
    case REGISTER_X8: return "s0/fp";
    case REGISTER_X9: return "s1";
    case REGISTER_X10: return "a0";
    case REGISTER_X11: return "a1";
    case REGISTER_X12: return "a2";
    case REGISTER_X13: return "a3";
    case REGISTER_X14: return "a4";
    case REGISTER_X15: return "a5";
    case REGISTER_X16: return "a6";
    case REGISTER_X17: return "a7";
    case REGISTER_X18: return "s2";
    case REGISTER_X19: return "s3";
    case REGISTER_X20: return "s4";
    case REGISTER_X21: return "s5";
    case REGISTER_X22: return "s6";
    case REGISTER_X23: return "s7";
    case REGISTER_X24: return "s8";
    case REGISTER_X25: return "s9";
    case REGISTER_X26: return "s10";
    case REGISTER_X27: return "s11";
    case REGISTER_X28: return "t3";
    case REGISTER_X29: return "t4";
    case REGISTER_X30: return "t5";
    case REGISTER_X31: return "t6";
    case REGISTER_PC: return "pc";
    default: return "unknown";
    }
}

void riscv32_dump_registers(riscv32_vm_state_t *vm)
{
    for ( int i = 0; i < REGISTERS_MAX - 1; i++ ) {
        printf("%-5s: 0x%08"PRIX32"  ", riscv32i_translate_register(i), riscv32i_read_register_u(vm, i));

        if (((i + 1) % 4) == 0)
            printf("\n");
    }
    printf("%-5s: 0x%08"PRIX32"\n", riscv32i_translate_register(32), riscv32i_read_register_u(vm, 32));
}

static void riscv32_trap_jump(riscv32_vm_state_t *vm)
{
    size_t pc = vm->csr.tvec[vm->priv_mode] & (~3);
    if (vm->csr.tvec[vm->priv_mode] & 1) pc += vm->csr.cause[vm->priv_mode] << 2;
    riscv32i_write_register_u(vm, REGISTER_PC, pc);
}

static void* riscv32_run_impl(void *arg)
{
    riscv32_vm_state_t *vm = (riscv32_vm_state_t *) arg;
    assert(vm);

    while (true) {
        atomic_store_explicit(&vm->wait_event, 1, memory_order_release);
        riscv32_run_till_event(vm);
        if (vm->ev_trap) {
            // Event came from CPU thread, either from trap or interrupted WFI
            vm->ev_trap = false;
            riscv32_trap_jump(vm);
        } else if (vm->ev_int) {
            // External interrupt, handle the pending bitmask
            vm->csr.ip |= vm->ev_int_mask;
            if (vm->csr.ip & (1 << INTERRUPT_MTIMER)) {
                if (!rvtimer_pending(&vm->timer)) {
                    vm->csr.ip &= ~(1 << INTERRUPT_MTIMER);
                }
            }
            vm->ev_int = false;
            if (riscv32_handle_ip(vm, false)) {
                riscv32_trap_jump(vm);
            }
        }
    }

    return NULL;
}

thread_handle_t riscv32_run(riscv32_vm_state_t *vm)
{
    return thread_create(&riscv32_run_impl, vm);
}

riscv32_vm_state_t* riscv32_get_hart_by_id(uint32_t hartid)
{
    if (hartid >= MAX_VMS) {
        return NULL;
    }

    return global_vm_list[hartid];
}
