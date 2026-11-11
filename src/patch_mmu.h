#ifndef _patch_mmu_h_
#define _patch_mmu_h_

// Don't include this header file directly, instead include patch.h.
// This header only has the parts required for patching via MMU.

#if !defined(MODULE) && !defined(CONFIG_MMU_REMAP)
    // We want to fail cam builds without CONFIG_MMU_REMAP,
    // but allow module builds (modules can run on both kinds of cams)
    #error "patch_mmu.h included but CONFIG_MMU_REMAP not defined, shouldn't happen"
#endif

#include "patch.h"

struct mmu_L2_page_info
{
    uint8_t *phys_mem[16]; // 16 possible 0x10000 pages of backing ram
    uint8_t *L2_table; // L2 table used for remapping into this page,
                       // size MMU_L2_TABLE_SIZE.
    uint32_t virt_page_mapped; // what 0x100000-aligned VA page addr is mapped here
    uint32_t in_use; // currently used to back a ROM->RAM mapping?
};

struct mmu_config
{
    uint8_t *L1_table; // single region of size MMU_TABLE_SIZE
    uint32_t max_L2_tables;
    struct mmu_L2_page_info *L2_tables; // struct, not direct access to L2 mem, so we can
                                        // easily track which pages are mapped where.
    uint32_t max_64k_pages_remapped;
    uint8_t *phys_mem_pages;
};

extern struct mmu_config global_mmu_conf; // in patch_mmu.c

// Space reserved in magiclantern.bin for remapped ROM pages (patch_mmu_space.c),
// when ML is not in the AllocMem pool: per page 0x10000 (0x10000 aligned) and
// one L2 table (0x400, 0x400 aligned, one per 1 MB region with remaps) with its
// metadata, plus one L1 table (0x4900, 0x4000 aligned) and 0x300 padding.
// A camera may reserve more pages (CONFIG_MMU_REMAP_PAGES, internals.h).
#ifndef CONFIG_MMU_REMAP_PAGES
#define CONFIG_MMU_REMAP_PAGES 1
#endif
#ifdef CONFIG_MMU_REMAP_SMALL_PAGES
// Fixed layout (calc_mmu_globals_small): L1 table at the 0x4000-aligned start,
// L2 tables after it (0x400 aligned) and their metadata. The 64 kB page copies
// come from the heap, 4 kB aligned (mapped with 4 kB small pages), so they
// take nothing from ML's boot reservation (README-XPAN-M50.md "4 KB pages").
#define GENERIC_MMU_SPACE_ALIGN 0x4000
#define GENERIC_MMU_SPACE_SIZE (MMU_L1_TABLE_SIZE + 0x300 \
                                + (MMU_L2_TABLE_SIZE + sizeof(struct mmu_L2_page_info)) * CONFIG_MMU_REMAP_PAGES)
#else
#define GENERIC_MMU_SPACE_ALIGN 0x10000
#define GENERIC_MMU_SPACE_SIZE ((MMU_PAGE_SIZE + MMU_L2_TABLE_SIZE + sizeof(struct mmu_L2_page_info)) * CONFIG_MMU_REMAP_PAGES + MMU_L1_TABLE_SIZE + 0x300)
#endif
extern void change_mmu_tables(uint8_t *ttbr0, uint8_t *ttbr1, uint32_t cpu_id);
void change_mmu_tables_wrapper(void);

int apply_patch(struct patch *patch);
int apply_normal_patches(void);
#if defined(CONFIG_MMU_REMAP_SMALL_PAGES) && !defined(CONFIG_ALLOCATE_MEMORY_POOL)
// Copies the ROM pages these patches will remap, interrupts on (patch_mmu.c).
void mmu_prefill_rom_pages(const struct patch *patches, uint32_t count);
#endif

// Sets up structures required for remapping via MMU,
// and applies compile-time specified patches from platform/XXD/include/platform/mmu_patches.h
int mmu_init(void);

#endif // _patch_mmu_h_
