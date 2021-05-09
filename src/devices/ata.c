#include "bit_ops.h"
#include "mem_ops.h"
#include "riscv32.h"
#include "riscv32_mmu.h"
#include "ata.h"
#include <string.h>
#include <inttypes.h>

/* Data registers */
#define ATA_REG_DATA 0x00
#define ATA_REG_ERR 0x01 /* or FEATURE */
#define ATA_REG_NSECT 0x02
#define ATA_REG_LBAL 0x03
#define ATA_REG_LBAM 0x04
#define ATA_REG_LBAH 0x05
#define ATA_REG_DEVICE 0x06
#define ATA_REG_STATUS 0x07 /* or CMD */

/* Control registers */
#define ATA_REG_CTL 0x00 /* or alternate STATUS */
#define ATA_REG_DRVADDR 0x01

/* 16-bit registers - needed for LBA48 */
#define ATA_REG_SHIFT 2
typedef uint16_t atareg_t;

/* Error flags for ERR register */
#define ATA_ERR_AMNF (1 << 0)
#define ATA_ERR_TKZNF (1 << 1)
#define ATA_ERR_ABRT (1 << 2)
#define ATA_ERR_MCR (1 << 3)
#define ATA_ERR_IDNF (1 << 4)
#define ATA_ERR_MC (1 << 5)
#define ATA_ERR_UNC (1 << 6)
#define ATA_ERR_BBK (1 << 7)

/* Flags for STATUS register */
#define ATA_STATUS_ERR (1 << 0)
#define ATA_STATUS_IDX (1 << 1)
#define ATA_STATUS_CORR (1 << 2)
#define ATA_STATUS_DRQ (1 << 3)
#define ATA_STATUS_SRV (1 << 4) /* or DSC aka Seek Complete, deprecated */
#define ATA_STATUS_DF (1 << 5)
#define ATA_STATUS_RDY (1 << 6)
#define ATA_STATUS_BSY (1 << 7)

/* Flags for DRIVE/HEAD register */
#define ATA_DRIVE_DRV (1 << 4)
#define ATA_DRIVE_LBA (1 << 6)

/* Commands */
#define ATA_CMD_IDENTIFY 0xEC
#define ATA_CMD_INITIALIZE_DEVICE_PARAMS 0x91
#define ATA_CMD_READ_SECTORS 0x20
#define ATA_CMD_WRITE_SECTORS 0x30

#define SECTOR_SIZE 512

/* CHS is not supported - it's dead anyway... */
#if 0
/* Limits for C/H/S calculation */
/* max sectors per track: 255, usually 63 because of six bit numberring. Starts from 1 */
#define MAX_SPT 63 /* sectors per track */
#define MAX_HPC 16 /* heads per cyllinder/track */
#define MAX_CYLLINDERS 65536 /* 16-bit addressing limit */
#endif

#define DIV_ROUND_UP(x, d) (((x) + (d) - 1) / (d))

struct ata_dev
{
    struct {
        FILE *fp;
        size_t size; // in sectors
        uint16_t bytes_to_rw;
        uint16_t sectcount;
        atareg_t lbal;
        atareg_t lbam;
        atareg_t lbah;
        atareg_t drive;
        atareg_t error;
        uint8_t status;
        uint8_t hob_shift;
        uint8_t buf[SECTOR_SIZE];
    } drive[2]; 
    uint8_t curdrive;
};

static uint64_t ata_get_lba(struct ata_dev *ata, bool is48bit)
{
    if (is48bit) {
        return bit_cut(ata->drive[ata->curdrive].lbal, 0, 8)
            | bit_cut(ata->drive[ata->curdrive].lbam, 0, 8) << 8
            | bit_cut(ata->drive[ata->curdrive].lbah, 0, 8) << 16
            | bit_cut(ata->drive[ata->curdrive].lbal, 8, 8) << 24
            | bit_cut(ata->drive[ata->curdrive].lbam, 8, 8) << 32
            | bit_cut(ata->drive[ata->curdrive].lbah, 8, 8) << 40;
    } else {
        return bit_cut(ata->drive[ata->curdrive].lbal, 0, 8)
            | bit_cut(ata->drive[ata->curdrive].lbam, 0, 8) << 8
            | bit_cut(ata->drive[ata->curdrive].lbah, 0, 8) << 16
            | bit_cut(ata->drive[ata->curdrive].drive, 0, 4) << 24;
    }
}

static void ata_cmd_identify(struct ata_dev *ata)
{
    uint16_t id_buf[SECTOR_SIZE / 2] = {
        [0] = (1 << 6), // non-removable, ATA device
        [1] = 65535, // logical cylinders
        [3] = 16, // logical heads
        [6] = 63, // sectors per track
        [22] = 4, // number of bytes available in READ/WRITE LONG cmds
        [47] = 0, // read-write multipe commands not implemented
        [49] = (1 << 9), // Capabilities - LBA supported
        [50] = (1 << 14), // Capabilities - bit 14 needs to be set as required by ATA/ATAPI-5 spec
        [51] = (4 << 8), // PIO data transfer cycle timing mode
        [53] = 1 | 2, // fields 54-58 and 64-70 are valid
        [54] = 65535, // logical cylinders
        [55] = 16, // logical heads
        [56] = 63, // sectors per track
        // capacity in sectors
        [57] = ata->drive[ata->curdrive].size > 0xffffffff ? 0xffff : ata->drive[ata->curdrive].size & 0xffff,
        [58] = ata->drive[ata->curdrive].size > 0xffffffff ? 0xffff : ata->drive[ata->curdrive].size >> 16,
        [60] = ata->drive[ata->curdrive].size > 0xffffffff ? 0xffff : ata->drive[ata->curdrive].size & 0xffff,
        [61] = ata->drive[ata->curdrive].size > 0xffffffff ? 0xffff : ata->drive[ata->curdrive].size >> 16,
        [64] = 1 | 2, // advanced PIO modes supported
        [67] = 1, // PIO transfer cycle time without flow control
        [68] = 1, // PIO transfer cycle time with IORDY flow control
    };

    memcpy(ata->drive[ata->curdrive].buf, id_buf, sizeof(id_buf));
    ata->drive[ata->curdrive].bytes_to_rw = sizeof(id_buf);
    ata->drive[ata->curdrive].status = ATA_STATUS_RDY | ATA_STATUS_SRV | ATA_STATUS_DRQ;
    ata->drive[ata->curdrive].sectcount = 1;
}

static void ata_cmd_initialize_device_params(struct ata_dev *ata)
{
    /* CHS translation is not supported */
    ata->drive[ata->curdrive].status |= ATA_STATUS_ERR;
    ata->drive[ata->curdrive].error |= ATA_ERR_ABRT;
}

/* Reads the data to buffer */
static bool ata_read_buf(struct ata_dev *ata)
{
    //printf("ATA fill next sector\n");
    if (1 != fread(ata->drive[ata->curdrive].buf,
                SECTOR_SIZE,
                1,
                ata->drive[ata->curdrive].fp)) {
        return false;
    }

    ata->drive[ata->curdrive].bytes_to_rw = SECTOR_SIZE;
    return true;
}

/* Writes the data to buffer */
static bool ata_write_buf(struct ata_dev *ata)
{
    //printf("ATA write buf\n");
    if (1 != fwrite(ata->drive[ata->curdrive].buf,
                SECTOR_SIZE,
                1,
                ata->drive[ata->curdrive].fp)) {
        return false;
    }

    return true;
}

static void ata_cmd_read_sectors(struct ata_dev *ata)
{
    /* Sector count of 0 means 256 */
    if (ata->drive[ata->curdrive].sectcount == 0) {
        ata->drive[ata->curdrive].sectcount = 256;
    }

    //printf("ATA read sectors count: %d offset: 0x%08"PRIx64"\n", ata->drive[ata->curdrive].sectcount, ata_get_lba(ata, false));

    ata->drive[ata->curdrive].status |= ATA_STATUS_DRQ | ATA_STATUS_RDY;
    if (fseek(ata->drive[ata->curdrive].fp,
                ata_get_lba(ata, false) * SECTOR_SIZE,
                SEEK_SET) < 0) {
        goto err;
    }

    if (!ata_read_buf(ata)) {
        goto err;
    }

    return;
err:
    ata->drive[ata->curdrive].status |= ATA_STATUS_ERR;
    ata->drive[ata->curdrive].error |= ATA_ERR_UNC;
}

static void ata_cmd_write_sectors(struct ata_dev *ata)
{
    /* Sector count of 0 means 256 */
    if (ata->drive[ata->curdrive].sectcount == 0) {
        ata->drive[ata->curdrive].sectcount = 256;
    }

    //printf("ATA write sectors count: %d offset: 0x%08"PRIx64"\n", ata->drive[ata->curdrive].sectcount, ata_get_lba(ata, false));
    ata->drive[ata->curdrive].status |= ATA_STATUS_DRQ | ATA_STATUS_RDY;
    if (fseek(ata->drive[ata->curdrive].fp,
                ata_get_lba(ata, false) * SECTOR_SIZE,
                SEEK_SET) < 0) {
        goto err;
    }

    ata->drive[ata->curdrive].bytes_to_rw = SECTOR_SIZE;
    return;
err:
    ata->drive[ata->curdrive].status |= ATA_STATUS_ERR;
    ata->drive[ata->curdrive].error |= ATA_ERR_UNC;
}

static void ata_handle_cmd(struct ata_dev *ata, uint8_t cmd)
{
    //printf("ATA command: 0x%02X\n", cmd);
    switch (cmd) {
        case ATA_CMD_IDENTIFY: ata_cmd_identify(ata); break;
        case ATA_CMD_INITIALIZE_DEVICE_PARAMS: ata_cmd_initialize_device_params(ata); break;
        case ATA_CMD_READ_SECTORS: ata_cmd_read_sectors(ata); break;
        case ATA_CMD_WRITE_SECTORS: ata_cmd_write_sectors(ata); break;
    }
}

static bool ata_data_mmio_handler(riscv32_vm_state_t* vm, riscv32_mmio_device_t* device, uint32_t offset, void* memory_data, uint32_t size, uint8_t access)
{
    UNUSED(vm);
    struct ata_dev *ata = (struct ata_dev *) device->data;

    if ((offset & ((1 << ATA_REG_SHIFT) - 1)) != 0) {
        /* TODO: misalign */
        return false;
    }

    offset >>= ATA_REG_SHIFT;
#if 0
    if (access == MMU_WRITE)
        printf("ATA DATA MMIO offset: %d size: %d write val: 0x%02X\n", offset, size, *(uint8_t*) memory_data);
    else
        printf("ATA DATA MMIO offset: %d size: %d read\n", offset, size);
#endif

    /* DATA register is of any size, others are 1 byte r/w */
    if (size != 1 && offset != ATA_REG_DATA) {
        /* TODO: misalign */
        return false;
    }

    switch (offset) {
        case ATA_REG_DATA:
            if (access == MMU_WRITE) {
                uint8_t *addr = ata->drive[ata->curdrive].buf + SECTOR_SIZE - ata->drive[ata->curdrive].bytes_to_rw;
                memcpy(addr, memory_data, size);

                ata->drive[ata->curdrive].bytes_to_rw -= size;
                if (ata->drive[ata->curdrive].bytes_to_rw == 0) {
                    ata->drive[ata->curdrive].status &= ~ATA_STATUS_DRQ;
                    if (--ata->drive[ata->curdrive].sectcount != 0) {
                        ata->drive[ata->curdrive].status |= ATA_STATUS_DRQ;
                        ata->drive[ata->curdrive].bytes_to_rw = SECTOR_SIZE;
                    }
                    if (!ata_write_buf(ata)) {
                        ata->drive[ata->curdrive].status |= ATA_STATUS_ERR;
                        ata->drive[ata->curdrive].error |= ATA_ERR_UNC;
                    }
                }
            } else {
                if (ata->drive[ata->curdrive].bytes_to_rw != 0) {
                    uint8_t *addr = ata->drive[ata->curdrive].buf + SECTOR_SIZE - ata->drive[ata->curdrive].bytes_to_rw;
#if 0
                    if (size == 4) {
                        write_uint32_le(memory_data, read_uint32_le(addr));
                    } else if (size == 2) {
                        write_uint16_le(memory_data, read_uint16_le(addr));
                    }
#endif
                    memcpy(memory_data, addr, size);

                    ata->drive[ata->curdrive].bytes_to_rw -= size;
                    if (ata->drive[ata->curdrive].bytes_to_rw == 0) {
                        ata->drive[ata->curdrive].status &= ~ATA_STATUS_DRQ;
                        if (--ata->drive[ata->curdrive].sectcount != 0) {
                            ata->drive[ata->curdrive].status |= ATA_STATUS_DRQ;
                            if (!ata_read_buf(ata)) {
                                ata->drive[ata->curdrive].status |= ATA_STATUS_ERR;
                                ata->drive[ata->curdrive].error |= ATA_ERR_UNC;
                            }
                        }
                    }
                } else {
                    memset(memory_data, '\0', size);
                }
            }
            break;
        case ATA_REG_ERR:
            if (access == MMU_WRITE) {
                /* FEATURES - ignore */
            } else {
                /* ERROR */

                /* OSDev says that this register is 16-bit,
                 * but there's no address stored so this seems wrong */
                memcpy(memory_data, &ata->drive[ata->curdrive].error, size);
            }
            break;
        case ATA_REG_NSECT:
            if (access == MMU_WRITE) {
                ata->drive[ata->curdrive].sectcount <<= 8;
                ata->drive[ata->curdrive].sectcount |= *(uint8_t*) memory_data;
            } else {
                *(uint8_t*) memory_data = (ata->drive[ata->curdrive].sectcount >> ata->drive[ata->curdrive].hob_shift) & 0xff;
            }
            break;
        case ATA_REG_LBAL:
            if (access == MMU_WRITE) {
                ata->drive[ata->curdrive].lbal <<= 8;
                ata->drive[ata->curdrive].lbal |= *(uint8_t*) memory_data;
            } else {
                *(uint8_t*) memory_data = (ata->drive[ata->curdrive].lbal >> ata->drive[ata->curdrive].hob_shift) & 0xff;
            }
            break;
        case ATA_REG_LBAM:
            if (access == MMU_WRITE) {
                ata->drive[ata->curdrive].lbam <<= 8;
                ata->drive[ata->curdrive].lbam |= *(uint8_t*) memory_data;
            } else {
                *(uint8_t*) memory_data = (ata->drive[ata->curdrive].lbam >> ata->drive[ata->curdrive].hob_shift) & 0xff;
            }
            break;
        case ATA_REG_LBAH:
            if (access == MMU_WRITE) {
                ata->drive[ata->curdrive].lbah <<= 8;
                ata->drive[ata->curdrive].lbah |= *(uint8_t*) memory_data;
            } else {
                *(uint8_t*) memory_data = (ata->drive[ata->curdrive].lbah >> ata->drive[ata->curdrive].hob_shift) & 0xff;
            }
            break;
        case ATA_REG_DEVICE:
            if (access == MMU_WRITE) {
                ata->curdrive = bit_check(*(uint8_t*) memory_data, 4) ? 1 : 0;
                ata->drive[ata->curdrive].drive = *(uint8_t*) memory_data;
            } else {
                *(uint8_t*) memory_data = ata->drive[ata->curdrive].drive | (1 << 5) | (1 << 7);
            }
            break;
        case ATA_REG_STATUS:
            if (access == MMU_WRITE) {
                /* Command */

                /* Not sure when error is cleared.
                 * Spec says that it contains status of the last command executed */
                ata->drive[ata->curdrive].error = 0;
                ata->drive[ata->curdrive].status &= ~ATA_STATUS_ERR;
                ata_handle_cmd(ata, *(uint8_t*) memory_data);
            } else {
                /* STATUS */
                *(uint8_t*) memory_data = ata->drive[ata->curdrive].status;
            }
            break;
    }

    return true;
}

static bool ata_ctl_mmio_handler(riscv32_vm_state_t* vm, riscv32_mmio_device_t* device, uint32_t offset, void* memory_data, uint32_t size, uint8_t access)
{
    UNUSED(vm);
    struct ata_dev *ata = (struct ata_dev *) device->data;

    if (size != 1 || (offset & ((1 << ATA_REG_SHIFT) - 1)) != 0) {
        /* TODO: misalign */
        return false;
    }

    offset >>= ATA_REG_SHIFT;
#if 0
    if (access == MMU_WRITE)
        printf("ATA CTL MMIO offset: %d size: %d write val: 0x%02X\n", offset, size, *(uint8_t*) memory_data);
    else
        printf("ATA CTL MMIO offset: %d size: %d read\n", offset, size);
#endif

    switch (offset) {
        case ATA_REG_CTL:
            if (access == MMU_READ) {
                /* Alternate STATUS */
                *(uint8_t*) memory_data = ata->drive[ata->curdrive].status;
            } else {
                /* Device control */
                ata->drive[ata->curdrive].hob_shift = bit_check(*(uint8_t*) memory_data, 7) ? 8 : 0;
                if (bit_check(*(uint8_t*) memory_data, 2)) {
                    /* Soft reset */
                    ata->drive[ata->curdrive].bytes_to_rw = 0;
                    ata->drive[ata->curdrive].lbal = 1; /* Sectors start from 1 */
                    ata->drive[ata->curdrive].lbah = 0;
                    ata->drive[ata->curdrive].lbam = 0;
                    ata->drive[ata->curdrive].sectcount = 1;
                    ata->drive[ata->curdrive].drive = 0;
                    if (ata->drive[ata->curdrive].fp != NULL) {
                        ata->drive[ata->curdrive].error = ATA_ERR_AMNF; /* AMNF means OK here... */
                        ata->drive[ata->curdrive].status = ATA_STATUS_RDY | ATA_STATUS_SRV;
                    } else {
                        ata->drive[ata->curdrive].error = 0;
                        ata->drive[ata->curdrive].status = 0;
                    }
                }
            }
            break;
        case ATA_REG_DRVADDR:
            /* TODO: seems that Linux doesn't use this */
            break;
    }

    return true;
}

static size_t get_img_size(FILE *fp)
{
    fseek(fp, 0, SEEK_END);

    long size = ftell(fp);
    if (size < 0) {
        printf("Unable to get ATA disk size\n");
        return 0;
    } else if (size == 0) {
        printf("ATA disk is empty");
        return 0;
    }

    fseek(fp, 0, SEEK_SET);
    return size;
}

void ata_init(riscv32_vm_state_t *vm, uint32_t data_base_addr, uint32_t ctl_base_addr, FILE *master, FILE *slave)
{
    assert(master != NULL || slave != NULL);
    struct ata_dev *ata = (struct ata_dev *) calloc(1, sizeof(struct ata_dev));
    ata->drive[0].fp = master;
    ata->drive[0].size = DIV_ROUND_UP(get_img_size(ata->drive[0].fp), SECTOR_SIZE);
    if (ata->drive[0].size == 0) {
        ata->drive[0].fp = NULL;
    }
    ata->drive[1].fp = slave;
    ata->drive[1].size = DIV_ROUND_UP(get_img_size(ata->drive[0].fp), SECTOR_SIZE);
    if (ata->drive[1].size == 0) {
        ata->drive[1].fp = NULL;
    }

    riscv32_mmio_add_device(vm,
            data_base_addr,
            data_base_addr + ((ATA_REG_STATUS + 1) << ATA_REG_SHIFT),
            ata_data_mmio_handler,
            ata);
    riscv32_mmio_add_device(vm,
            ctl_base_addr,
            ctl_base_addr + ((ATA_REG_DRVADDR + 1) << ATA_REG_SHIFT),
            ata_ctl_mmio_handler,
            ata);
}
