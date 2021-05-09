#ifndef ATA_H
#define ATA_H

#include "riscv32.h"
void ata_init(riscv32_vm_state_t *vm, uint32_t data_base_addr, uint32_t ctl_base_addr, FILE *fp0, FILE *fp1);

#endif
