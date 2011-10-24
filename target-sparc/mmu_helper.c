/*
 *  Sparc MMU helpers
 *
 *  Copyright (c) 2003-2005 Fabrice Bellard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "cpu.h"
#include "trace.h"

/* Sparc MMU emulation */

#if defined(CONFIG_USER_ONLY)

int cpu_sparc_handle_mmu_fault(CPUState *env1, target_ulong address, int rw,
                               int mmu_idx)
{
    if (rw & 2) {
        env1->exception_index = TT_TFAULT;
    } else {
        env1->exception_index = TT_DFAULT;
    }
    return 1;
}

#else

#ifndef TARGET_SPARC64
/*
 * Sparc V8 Reference MMU (SRMMU)
 */
static const int access_table[8][8] = {
    { 0, 0, 0, 0, 8, 0, 12, 12 },
    { 0, 0, 0, 0, 8, 0, 0, 0 },
    { 8, 8, 0, 0, 0, 8, 12, 12 },
    { 8, 8, 0, 0, 0, 8, 0, 0 },
    { 8, 0, 8, 0, 8, 8, 12, 12 },
    { 8, 0, 8, 0, 8, 0, 8, 0 },
    { 8, 8, 8, 0, 8, 8, 12, 12 },
    { 8, 8, 8, 0, 8, 8, 8, 0 }
};

static const int perm_table[2][8] = {
    {
        PAGE_READ,
        PAGE_READ | PAGE_WRITE,
        PAGE_READ | PAGE_EXEC,
        PAGE_READ | PAGE_WRITE | PAGE_EXEC,
        PAGE_EXEC,
        PAGE_READ | PAGE_WRITE,
        PAGE_READ | PAGE_EXEC,
        PAGE_READ | PAGE_WRITE | PAGE_EXEC
    },
    {
        PAGE_READ,
        PAGE_READ | PAGE_WRITE,
        PAGE_READ | PAGE_EXEC,
        PAGE_READ | PAGE_WRITE | PAGE_EXEC,
        PAGE_EXEC,
        PAGE_READ,
        0,
        0,
    }
};

static int get_physical_address(CPUState *env, target_phys_addr_t *physical,
                                int *prot, int *access_index,
                                target_ulong address, int rw, int mmu_idx,
                                target_ulong *page_size)
{
    int access_perms = 0;
    target_phys_addr_t pde_ptr;
    uint32_t pde;
    int error_code = 0, is_dirty, is_user;
    unsigned long page_offset;

    is_user = mmu_idx == MMU_USER_IDX;

    if ((env->mmuregs[0] & MMU_E) == 0) { /* MMU disabled */
        *page_size = TARGET_PAGE_SIZE;
        /* Boot mode: instruction fetches are taken from PROM */
        if (rw == 2 && (env->mmuregs[0] & env->def->mmu_bm)) {
            *physical = env->prom_addr | (address & 0x7ffffULL);
            *prot = PAGE_READ | PAGE_EXEC;
            return 0;
        }
        *physical = address;
        *prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        return 0;
    }

    *access_index = ((rw & 1) << 2) | (rw & 2) | (is_user ? 0 : 1);
    *physical = 0xffffffffffff0000ULL;

    /* SPARC reference MMU table walk: Context table->L1->L2->PTE */
    /* Context base + context number */
    pde_ptr = (env->mmuregs[1] << 4) + (env->mmuregs[2] << 2);
    pde = ldl_phys(pde_ptr);

    /* Ctx pde */
    switch (pde & PTE_ENTRYTYPE_MASK) {
    default:
    case 0: /* Invalid */
        return 1 << 2;
    case 2: /* L0 PTE, maybe should not happen? */
    case 3: /* Reserved */
        return 4 << 2;
    case 1: /* L0 PDE */
        pde_ptr = ((address >> 22) & ~3) + ((pde & ~3) << 4);
        pde = ldl_phys(pde_ptr);

        switch (pde & PTE_ENTRYTYPE_MASK) {
        default:
        case 0: /* Invalid */
            return (1 << 8) | (1 << 2);
        case 3: /* Reserved */
            return (1 << 8) | (4 << 2);
        case 1: /* L1 PDE */
            pde_ptr = ((address & 0xfc0000) >> 16) + ((pde & ~3) << 4);
            pde = ldl_phys(pde_ptr);

            switch (pde & PTE_ENTRYTYPE_MASK) {
            default:
            case 0: /* Invalid */
                return (2 << 8) | (1 << 2);
            case 3: /* Reserved */
                return (2 << 8) | (4 << 2);
            case 1: /* L2 PDE */
                pde_ptr = ((address & 0x3f000) >> 10) + ((pde & ~3) << 4);
                pde = ldl_phys(pde_ptr);

                switch (pde & PTE_ENTRYTYPE_MASK) {
                default:
                case 0: /* Invalid */
                    return (3 << 8) | (1 << 2);
                case 1: /* PDE, should not happen */
                case 3: /* Reserved */
                    return (3 << 8) | (4 << 2);
                case 2: /* L3 PTE */
                    page_offset = (address & TARGET_PAGE_MASK) &
                        (TARGET_PAGE_SIZE - 1);
                }
                *page_size = TARGET_PAGE_SIZE;
                break;
            case 2: /* L2 PTE */
                page_offset = address & 0x3ffff;
                *page_size = 0x40000;
            }
            break;
        case 2: /* L1 PTE */
            page_offset = address & 0xffffff;
            *page_size = 0x1000000;
        }
    }

    /* check access */
    access_perms = (pde & PTE_ACCESS_MASK) >> PTE_ACCESS_SHIFT;
    error_code = access_table[*access_index][access_perms];
    if (error_code && !((env->mmuregs[0] & MMU_NF) && is_user)) {
        return error_code;
    }

    /* update page modified and dirty bits */
    is_dirty = (rw & 1) && !(pde & PG_MODIFIED_MASK);
    if (!(pde & PG_ACCESSED_MASK) || is_dirty) {
        pde |= PG_ACCESSED_MASK;
        if (is_dirty) {
            pde |= PG_MODIFIED_MASK;
        }
        stl_phys_notdirty(pde_ptr, pde);
    }

    /* the page can be put in the TLB */
    *prot = perm_table[is_user][access_perms];
    if (!(pde & PG_MODIFIED_MASK)) {
        /* only set write access if already dirty... otherwise wait
           for dirty access */
        *prot &= ~PAGE_WRITE;
    }

    /* Even if large ptes, we map only one 4KB page in the cache to
       avoid filling it too fast */
    *physical = ((target_phys_addr_t)(pde & PTE_ADDR_MASK) << 4) + page_offset;
    return error_code;
}

/* Perform address translation */
int cpu_sparc_handle_mmu_fault(CPUState *env, target_ulong address, int rw,
                               int mmu_idx)
{
    target_phys_addr_t paddr;
    target_ulong vaddr;
    target_ulong page_size;
    int error_code = 0, prot, access_index;

    error_code = get_physical_address(env, &paddr, &prot, &access_index,
                                      address, rw, mmu_idx, &page_size);
    if (error_code == 0) {
        vaddr = address & TARGET_PAGE_MASK;
        paddr &= TARGET_PAGE_MASK;
#ifdef DEBUG_MMU
        printf("Translate at " TARGET_FMT_lx " -> " TARGET_FMT_plx ", vaddr "
               TARGET_FMT_lx "\n", address, paddr, vaddr);
#endif
        tlb_set_page(env, vaddr, paddr, prot, mmu_idx, page_size);
        return 0;
    }

    if (env->mmuregs[3]) { /* Fault status register */
        env->mmuregs[3] = 1; /* overflow (not read before another fault) */
    }
    env->mmuregs[3] |= (access_index << 5) | error_code | 2;
    env->mmuregs[4] = address; /* Fault address register */

    if ((env->mmuregs[0] & MMU_NF) || env->psret == 0)  {
        /* No fault mode: if a mapping is available, just override
           permissions. If no mapping is available, redirect accesses to
           neverland. Fake/overridden mappings will be flushed when
           switching to normal mode. */
        vaddr = address & TARGET_PAGE_MASK;
        prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;
        tlb_set_page(env, vaddr, paddr, prot, mmu_idx, TARGET_PAGE_SIZE);
        return 0;
    } else {
        if (rw & 2) {
            env->exception_index = TT_TFAULT;
        } else {
            env->exception_index = TT_DFAULT;
        }
        return 1;
    }
}

target_ulong mmu_probe(CPUState *env, target_ulong address, int mmulev)
{
    target_phys_addr_t pde_ptr;
    uint32_t pde;

    /* Context base + context number */
    pde_ptr = (target_phys_addr_t)(env->mmuregs[1] << 4) +
        (env->mmuregs[2] << 2);
    pde = ldl_phys(pde_ptr);

    switch (pde & PTE_ENTRYTYPE_MASK) {
    default:
    case 0: /* Invalid */
    case 2: /* PTE, maybe should not happen? */
    case 3: /* Reserved */
        return 0;
    case 1: /* L1 PDE */
        if (mmulev == 3) {
            return pde;
        }
        pde_ptr = ((address >> 22) & ~3) + ((pde & ~3) << 4);
        pde = ldl_phys(pde_ptr);

        switch (pde & PTE_ENTRYTYPE_MASK) {
        default:
        case 0: /* Invalid */
        case 3: /* Reserved */
            return 0;
        case 2: /* L1 PTE */
            return pde;
        case 1: /* L2 PDE */
            if (mmulev == 2) {
                return pde;
            }
            pde_ptr = ((address & 0xfc0000) >> 16) + ((pde & ~3) << 4);
            pde = ldl_phys(pde_ptr);

            switch (pde & PTE_ENTRYTYPE_MASK) {
            default:
            case 0: /* Invalid */
            case 3: /* Reserved */
                return 0;
            case 2: /* L2 PTE */
                return pde;
            case 1: /* L3 PDE */
                if (mmulev == 1) {
                    return pde;
                }
                pde_ptr = ((address & 0x3f000) >> 10) + ((pde & ~3) << 4);
                pde = ldl_phys(pde_ptr);

                switch (pde & PTE_ENTRYTYPE_MASK) {
                default:
                case 0: /* Invalid */
                case 1: /* PDE, should not happen */
                case 3: /* Reserved */
                    return 0;
                case 2: /* L3 PTE */
                    return pde;
                }
            }
        }
    }
    return 0;
}

void dump_mmu(FILE *f, fprintf_function cpu_fprintf, CPUState *env)
{
    target_ulong va, va1, va2;
    unsigned int n, m, o;
    target_phys_addr_t pde_ptr, pa;
    uint32_t pde;

    pde_ptr = (env->mmuregs[1] << 4) + (env->mmuregs[2] << 2);
    pde = ldl_phys(pde_ptr);
    (*cpu_fprintf)(f, "Root ptr: " TARGET_FMT_plx ", ctx: %d\n",
                   (target_phys_addr_t)env->mmuregs[1] << 4, env->mmuregs[2]);
    for (n = 0, va = 0; n < 256; n++, va += 16 * 1024 * 1024) {
        pde = mmu_probe(env, va, 2);
        if (pde) {
            pa = cpu_get_phys_page_debug(env, va);
            (*cpu_fprintf)(f, "VA: " TARGET_FMT_lx ", PA: " TARGET_FMT_plx
                           " PDE: " TARGET_FMT_lx "\n", va, pa, pde);
            for (m = 0, va1 = va; m < 64; m++, va1 += 256 * 1024) {
                pde = mmu_probe(env, va1, 1);
                if (pde) {
                    pa = cpu_get_phys_page_debug(env, va1);
                    (*cpu_fprintf)(f, " VA: " TARGET_FMT_lx ", PA: "
                                   TARGET_FMT_plx " PDE: " TARGET_FMT_lx "\n",
                                   va1, pa, pde);
                    for (o = 0, va2 = va1; o < 64; o++, va2 += 4 * 1024) {
                        pde = mmu_probe(env, va2, 0);
                        if (pde) {
                            pa = cpu_get_phys_page_debug(env, va2);
                            (*cpu_fprintf)(f, "  VA: " TARGET_FMT_lx ", PA: "
                                           TARGET_FMT_plx " PTE: "
                                           TARGET_FMT_lx "\n",
                                           va2, pa, pde);
                        }
                    }
                }
            }
        }
    }
}

/* Gdb expects all registers windows to be flushed in ram. This function handles
 * reads (and only reads) in stack frames as if windows were flushed. We assume
 * that the sparc ABI is followed.
 */
int target_memory_rw_debug(CPUState *env, target_ulong addr,
                           uint8_t *buf, int len, int is_write)
{
    int i;
    int len1;
    int cwp = env->cwp;

    if (!is_write) {
        for (i = 0; i < env->nwindows; i++) {
            int off;
            target_ulong fp = env->regbase[cwp * 16 + 22];

            /* Assume fp == 0 means end of frame.  */
            if (fp == 0) {
                break;
            }

            cwp = cpu_cwp_inc(env, cwp + 1);

            /* Invalid window ? */
            if (env->wim & (1 << cwp)) {
                break;
            }

            /* According to the ABI, the stack is growing downward.  */
            if (addr + len < fp) {
                break;
            }

            /* Not in this frame.  */
            if (addr > fp + 64) {
                continue;
            }

            /* Handle access before this window.  */
            if (addr < fp) {
                len1 = fp - addr;
                if (cpu_memory_rw_debug(env, addr, buf, len1, is_write) != 0) {
                    return -1;
                }
                addr += len1;
                len -= len1;
                buf += len1;
            }

            /* Access byte per byte to registers. Not very efficient but speed
             * is not critical.
             */
            off = addr - fp;
            len1 = 64 - off;

            if (len1 > len) {
                len1 = len;
            }

            for (; len1; len1--) {
                int reg = cwp * 16 + 8 + (off >> 2);
                union {
                    uint32_t v;
                    uint8_t c[4];
                } u;
                u.v = cpu_to_be32(env->regbase[reg]);
                *buf++ = u.c[off & 3];
                addr++;
                len--;
                off++;
            }

            if (len == 0) {
                return 0;
            }
        }
    }
    return cpu_memory_rw_debug(env, addr, buf, len, is_write);
}

#else /* !TARGET_SPARC64 */

/* 41 bit physical address space */
static inline target_phys_addr_t ultrasparc_truncate_physical(uint64_t x)
{
    return x & 0x1ffffffffffULL;
}

/*
 * UltraSparc IIi I/DMMUs
 */

/* Returns true if TTE tag is valid and matches virtual address value
   in context requires virtual address mask value calculated from TTE
   entry size */
static inline int ultrasparc_tag_match(SparcTLBEntry *tlb,
                                       uint64_t address, uint64_t context,
                                       target_phys_addr_t *physical)
{
    uint64_t mask;

    switch (TTE_PGSIZE(tlb->tte)) {
    default:
    case 0x0: /* 8k */
        mask = 0xffffffffffffe000ULL;
        break;
    case 0x1: /* 64k */
        mask = 0xffffffffffff0000ULL;
        break;
    case 0x2: /* 512k */
        mask = 0xfffffffffff80000ULL;
        break;
    case 0x3: /* 4M */
        mask = 0xffffffffffc00000ULL;
        break;
    }

    /* valid, context match, virtual address match? */
    if (TTE_IS_VALID(tlb->tte) &&
        (TTE_IS_GLOBAL(tlb->tte) || tlb_compare_context(tlb, context))
        && compare_masked(address, tlb->tag, mask)) {
        /* decode physical address */
        *physical = ((tlb->tte & mask) | (address & ~mask)) & 0x1ffffffe000ULL;
        return 1;
    }

    return 0;
}

static int get_physical_address_data(CPUState *env,
                                     target_phys_addr_t *physical, int *prot,
                                     target_ulong address, int rw, int mmu_idx)
{
    unsigned int i;
    uint64_t context;
    uint64_t sfsr = 0;

    int is_user = (mmu_idx == MMU_USER_IDX ||
                   mmu_idx == MMU_USER_SECONDARY_IDX);

    if ((env->lsu & DMMU_E) == 0) { /* DMMU disabled */
        *physical = ultrasparc_truncate_physical(address);
        *prot = PAGE_READ | PAGE_WRITE;
        return 0;
    }

    switch (mmu_idx) {
    case MMU_USER_IDX:
    case MMU_KERNEL_IDX:
        context = env->dmmu.mmu_primary_context & 0x1fff;
        sfsr |= SFSR_CT_PRIMARY;
        break;
    case MMU_USER_SECONDARY_IDX:
    case MMU_KERNEL_SECONDARY_IDX:
        context = env->dmmu.mmu_secondary_context & 0x1fff;
        sfsr |= SFSR_CT_SECONDARY;
        break;
    case MMU_NUCLEUS_IDX:
        sfsr |= SFSR_CT_NUCLEUS;
        /* FALLTHRU */
    default:
        context = 0;
        break;
    }

    if (rw == 1) {
        sfsr |= SFSR_WRITE_BIT;
    } else if (rw == 4) {
        sfsr |= SFSR_NF_BIT;
    }

    for (i = 0; i < 64; i++) {
        /* ctx match, vaddr match, valid? */
        if (ultrasparc_tag_match(&env->dtlb[i], address, context, physical)) {
            int do_fault = 0;

            /* access ok? */
            /* multiple bits in SFSR.FT may be set on TT_DFAULT */
            if (TTE_IS_PRIV(env->dtlb[i].tte) && is_user) {
                do_fault = 1;
                sfsr |= SFSR_FT_PRIV_BIT; /* privilege violation */
                trace_mmu_helper_dfault(address, context, mmu_idx, env->tl);
            }
            if (rw == 4) {
                if (TTE_IS_SIDEEFFECT(env->dtlb[i].tte)) {
                    do_fault = 1;
                    sfsr |= SFSR_FT_NF_E_BIT;
                }
            } else {
                if (TTE_IS_NFO(env->dtlb[i].tte)) {
                    do_fault = 1;
                    sfsr |= SFSR_FT_NFO_BIT;
                }
            }

            if (do_fault) {
                /* faults above are reported with TT_DFAULT. */
                env->exception_index = TT_DFAULT;
            } else if (!TTE_IS_W_OK(env->dtlb[i].tte) && (rw == 1)) {
                do_fault = 1;
                env->exception_index = TT_DPROT;

                trace_mmu_helper_dprot(address, context, mmu_idx, env->tl);
            }

            if (!do_fault) {
                *prot = PAGE_READ;
                if (TTE_IS_W_OK(env->dtlb[i].tte)) {
                    *prot |= PAGE_WRITE;
                }

                TTE_SET_USED(env->dtlb[i].tte);

                return 0;
            }

            if (env->dmmu.sfsr & SFSR_VALID_BIT) { /* Fault status register */
                sfsr |= SFSR_OW_BIT; /* overflow (not read before
                                        another fault) */
            }

            if (env->pstate & PS_PRIV) {
                sfsr |= SFSR_PR_BIT;
            }

            /* FIXME: ASI field in SFSR must be set */
            env->dmmu.sfsr = sfsr | SFSR_VALID_BIT;

            env->dmmu.sfar = address; /* Fault address register */

            env->dmmu.tag_access = (address & ~0x1fffULL) | context;

            return 1;
        }
    }

    trace_mmu_helper_dmiss(address, context);

    /*
     * On MMU misses:
     * - UltraSPARC IIi: SFSR and SFAR unmodified
     * - JPS1: SFAR updated and some fields of SFSR updated
     */
    env->dmmu.tag_access = (address & ~0x1fffULL) | context;
    env->exception_index = TT_DMISS;
    return 1;
}

static int get_physical_address_code(CPUState *env,
                                     target_phys_addr_t *physical, int *prot,
                                     target_ulong address, int mmu_idx)
{
    unsigned int i;
    uint64_t context;

    int is_user = (mmu_idx == MMU_USER_IDX ||
                   mmu_idx == MMU_USER_SECONDARY_IDX);

    if ((env->lsu & IMMU_E) == 0 || (env->pstate & PS_RED) != 0) {
        /* IMMU disabled */
        *physical = ultrasparc_truncate_physical(address);
        *prot = PAGE_EXEC;
        return 0;
    }

    if (env->tl == 0) {
        /* PRIMARY context */
        context = env->dmmu.mmu_primary_context & 0x1fff;
    } else {
        /* NUCLEUS context */
        context = 0;
    }

    for (i = 0; i < 64; i++) {
        /* ctx match, vaddr match, valid? */
        if (ultrasparc_tag_match(&env->itlb[i],
                                 address, context, physical)) {
            /* access ok? */
            if (TTE_IS_PRIV(env->itlb[i].tte) && is_user) {
                /* Fault status register */
                if (env->immu.sfsr & SFSR_VALID_BIT) {
                    env->immu.sfsr = SFSR_OW_BIT; /* overflow (not read before
                                                     another fault) */
                } else {
                    env->immu.sfsr = 0;
                }
                if (env->pstate & PS_PRIV) {
                    env->immu.sfsr |= SFSR_PR_BIT;
                }
                if (env->tl > 0) {
                    env->immu.sfsr |= SFSR_CT_NUCLEUS;
                }

                /* FIXME: ASI field in SFSR must be set */
                env->immu.sfsr |= SFSR_FT_PRIV_BIT | SFSR_VALID_BIT;
                env->exception_index = TT_TFAULT;

                env->immu.tag_access = (address & ~0x1fffULL) | context;

                trace_mmu_helper_tfault(address, context);

                return 1;
            }
            *prot = PAGE_EXEC;
            TTE_SET_USED(env->itlb[i].tte);
            return 0;
        }
    }

    trace_mmu_helper_tmiss(address, context);

    /* Context is stored in DMMU (dmmuregs[1]) also for IMMU */
    env->immu.tag_access = (address & ~0x1fffULL) | context;
    env->exception_index = TT_TMISS;
    return 1;
}

static int get_physical_address(CPUState *env, target_phys_addr_t *physical,
                                int *prot, int *access_index,
                                target_ulong address, int rw, int mmu_idx,
                                target_ulong *page_size)
{
    /* ??? We treat everything as a small page, then explicitly flush
       everything when an entry is evicted.  */
    *page_size = TARGET_PAGE_SIZE;

    /* safety net to catch wrong softmmu index use from dynamic code */
    if (env->tl > 0 && mmu_idx != MMU_NUCLEUS_IDX) {
        if (rw == 2) {
            trace_mmu_helper_get_phys_addr_code(env->tl, mmu_idx,
                                                env->dmmu.mmu_primary_context,
                                                env->dmmu.mmu_secondary_context,
                                                address);
        } else {
            trace_mmu_helper_get_phys_addr_data(env->tl, mmu_idx,
                                                env->dmmu.mmu_primary_context,
                                                env->dmmu.mmu_secondary_context,
                                                address);
        }
    }

    if (rw == 2) {
        return get_physical_address_code(env, physical, prot, address,
                                         mmu_idx);
    } else {
        return get_physical_address_data(env, physical, prot, address, rw,
                                         mmu_idx);
    }
}

/* Perform address translation */
int cpu_sparc_handle_mmu_fault(CPUState *env, target_ulong address, int rw,
                               int mmu_idx)
{
    target_ulong virt_addr, vaddr;
    target_phys_addr_t paddr;
    target_ulong page_size;
    int error_code = 0, prot, access_index;

    error_code = get_physical_address(env, &paddr, &prot, &access_index,
                                      address, rw, mmu_idx, &page_size);
    if (error_code == 0) {
        virt_addr = address & TARGET_PAGE_MASK;
        vaddr = virt_addr + ((address & TARGET_PAGE_MASK) &
                             (TARGET_PAGE_SIZE - 1));

        trace_mmu_helper_mmu_fault(address, paddr, mmu_idx, env->tl,
                                   env->dmmu.mmu_primary_context,
                                   env->dmmu.mmu_secondary_context);

        tlb_set_page(env, vaddr, paddr, prot, mmu_idx, page_size);
        return 0;
    }
    /* XXX */
    return 1;
}

void dump_mmu(FILE *f, fprintf_function cpu_fprintf, CPUState *env)
{
    unsigned int i;
    const char *mask;

    (*cpu_fprintf)(f, "MMU contexts: Primary: %" PRId64 ", Secondary: %"
                   PRId64 "\n",
                   env->dmmu.mmu_primary_context,
                   env->dmmu.mmu_secondary_context);
    if ((env->lsu & DMMU_E) == 0) {
        (*cpu_fprintf)(f, "DMMU disabled\n");
    } else {
        (*cpu_fprintf)(f, "DMMU dump\n");
        for (i = 0; i < 64; i++) {
            switch (TTE_PGSIZE(env->dtlb[i].tte)) {
            default:
            case 0x0:
                mask = "  8k";
                break;
            case 0x1:
                mask = " 64k";
                break;
            case 0x2:
                mask = "512k";
                break;
            case 0x3:
                mask = "  4M";
                break;
            }
            if (TTE_IS_VALID(env->dtlb[i].tte)) {
                (*cpu_fprintf)(f, "[%02u] VA: %" PRIx64 ", PA: %llx"
                               ", %s, %s, %s, %s, ctx %" PRId64 " %s\n",
                               i,
                               env->dtlb[i].tag & (uint64_t)~0x1fffULL,
                               TTE_PA(env->dtlb[i].tte),
                               mask,
                               TTE_IS_PRIV(env->dtlb[i].tte) ? "priv" : "user",
                               TTE_IS_W_OK(env->dtlb[i].tte) ? "RW" : "RO",
                               TTE_IS_LOCKED(env->dtlb[i].tte) ?
                               "locked" : "unlocked",
                               env->dtlb[i].tag & (uint64_t)0x1fffULL,
                               TTE_IS_GLOBAL(env->dtlb[i].tte) ?
                               "global" : "local");
            }
        }
    }
    if ((env->lsu & IMMU_E) == 0) {
        (*cpu_fprintf)(f, "IMMU disabled\n");
    } else {
        (*cpu_fprintf)(f, "IMMU dump\n");
        for (i = 0; i < 64; i++) {
            switch (TTE_PGSIZE(env->itlb[i].tte)) {
            default:
            case 0x0:
                mask = "  8k";
                break;
            case 0x1:
                mask = " 64k";
                break;
            case 0x2:
                mask = "512k";
                break;
            case 0x3:
                mask = "  4M";
                break;
            }
            if (TTE_IS_VALID(env->itlb[i].tte)) {
                (*cpu_fprintf)(f, "[%02u] VA: %" PRIx64 ", PA: %llx"
                               ", %s, %s, %s, ctx %" PRId64 " %s\n",
                               i,
                               env->itlb[i].tag & (uint64_t)~0x1fffULL,
                               TTE_PA(env->itlb[i].tte),
                               mask,
                               TTE_IS_PRIV(env->itlb[i].tte) ? "priv" : "user",
                               TTE_IS_LOCKED(env->itlb[i].tte) ?
                               "locked" : "unlocked",
                               env->itlb[i].tag & (uint64_t)0x1fffULL,
                               TTE_IS_GLOBAL(env->itlb[i].tte) ?
                               "global" : "local");
            }
        }
    }
}

#endif /* TARGET_SPARC64 */

static int cpu_sparc_get_phys_page(CPUState *env, target_phys_addr_t *phys,
                                   target_ulong addr, int rw, int mmu_idx)
{
    target_ulong page_size;
    int prot, access_index;

    return get_physical_address(env, phys, &prot, &access_index, addr, rw,
                                mmu_idx, &page_size);
}

#if defined(TARGET_SPARC64)
target_phys_addr_t cpu_get_phys_page_nofault(CPUState *env, target_ulong addr,
                                           int mmu_idx)
{
    target_phys_addr_t phys_addr;

    if (cpu_sparc_get_phys_page(env, &phys_addr, addr, 4, mmu_idx) != 0) {
        return -1;
    }
    return phys_addr;
}
#endif

target_phys_addr_t cpu_get_phys_page_debug(CPUState *env, target_ulong addr)
{
    target_phys_addr_t phys_addr;
    int mmu_idx = cpu_mmu_index(env);

    if (cpu_sparc_get_phys_page(env, &phys_addr, addr, 2, mmu_idx) != 0) {
        if (cpu_sparc_get_phys_page(env, &phys_addr, addr, 0, mmu_idx) != 0) {
            return -1;
        }
    }
    if (cpu_get_physical_page_desc(phys_addr) == IO_MEM_UNASSIGNED) {
        return -1;
    }
    return phys_addr;
}
#endif