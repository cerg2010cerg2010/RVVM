
# RVVM - The RISC-V Virtual Machine

![RISC-V Logo](https://riscv.org/wp-content/uploads/2018/09/riscv-logo-1.png "The “RISC-V” trade name is a registered trade mark of RISC-V International.")


RISC-V CPU & System software implementation written in С

## What's working
- OpenSBI, custom firmwares boot and execute properly
- Linux kernel boots! Although, without clint & timer it does not run init

## What's done so far
- Feature-complete RV32I instruction set
- C, M, A instruction extensions
- Extendable and fast instruction decoder
- Physical memory
- Memory mapping unit (MMU) with SV32 virtual addressing
- TLB address caching (greatly speeds up memory operations)
- MMIO handlers
- CSR operations
- UART 16550a-compatible text console
- Bootrom loading
- DTB loading, passing to firmware/kernel
- ELF kernel loading

## Usage
Currently builds on *nix systems using GNU Make. Actual code however is cross-platform and more build targets are going to be supported, including Windows, or even embedded systems.
The bootrom.bin file is a user-provided raw binary, loaded at 0x80000000 address where it starts execution, and device.dtb is a DTB file containing description of the machine.
```
git clone https://github.com/LekKit/RVVM
cd RVVM
make
cd release.linux.x86_64
./rvvm_x86_64 bootrom.bin -dtb=device.dtb
```

## Our team
- **LekKit**:  Instruction decoding, RAM/MMU/TLB implementation, RV32ICA ISA, privileged ISA, lots of fixes
- **Mr0maks**: Initial ideas, C/M extensions, VM debugger, CSR work
- **cerg2010cerg2010**: ELF loading, important fixes and refactoring
- *Hoping to see more contributors here*

## TODO
- Debug the available functionality and make sure it's conforming to the specs
- Timer, CLINT
- Successfully running Linux userspace software (currently hangs at "Freeing unused kernel memory; Run /init as init process")
- Floating-point extensions
- Refactor the code; Make internal APIs usable for both 32 & 64 bit VMs
- Basic peripherals, some kind of storage
- *A lot more...*
- JIT? RV64? Userspace emulation?
