// RAM for patch_mmu.c when it is not taken from the AllocMem pool:
// remapped 64 kB ROM pages, the L1 table, L2 tables and their metadata.
//
// The array needs 64 kB alignment. In its own object file that alignment
// pads only this array's .bss; inside patch_mmu.o it also forced the
// whole patch_mmu.o .bss (small variables first) to 64 kB, so the linker
// padded twice (about 128 kB lost on the M50, README-XPAN-M50.md
// "Memory for a second hooked page"). calc_mmu_globals() handles any
// start and size; the alignment only avoids losing a page to it.

#include <dryos.h>
#include "patch.h"
#include "mmu_utils.h"

#if defined(CONFIG_MMU_REMAP) && !defined(CONFIG_ALLOCATE_MEMORY_POOL)
uint8_t generic_mmu_space[GENERIC_MMU_SPACE_SIZE]
               __attribute__((aligned(GENERIC_MMU_SPACE_ALIGN)));
#endif
