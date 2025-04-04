// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 SUSE LLC
 * Author: Nicolai Stange <nstange@suse.de>
 * LTP port: Martin Doucha <mdoucha@suse.cz>
 */

/*\
 * CVE 2021-38198
 *
 * Check that x86_64 KVM correctly enforces (lack of) write permissions
 * in 4-level and 5-level memory page table mode. Missing page faults fixed in:
 *
 *  commit b1bd5cba3306691c771d558e94baa73e8b0b96b7
 *  Author: Lai Jiangshan <laijs@linux.alibaba.com>
 *  Date:   Thu Jun 3 13:24:55 2021 +0800
 *
 *  KVM: X86: MMU: Use the correct inherited permissions to get shadow page
 */

#include "kvm_test.h"

#ifdef COMPILE_PAYLOAD
#ifdef __x86_64__

#include "kvm_x86.h"

#define PTE_BITMASK 0x1ff
#define PAGESIZE 0x1000

int handle_page_fault(void *userdata, struct kvm_interrupt_frame *ifrm,
	unsigned long errcode)
{
	struct kvm_cregs buf;

	kvm_read_cregs(&buf);

	/* Check that the page fault was caused by write to *readonly below */
	if (buf.cr2 == (uintptr_t)userdata) {
		tst_res(TPASS, "KVM enforces memory write permissions");
		kvm_exit();
	}

	/* Unexpected page fault, fall back to default handler */
	return 0;
}

void main(void)
{
	uintptr_t tmp;
	struct page_table_entry_pae *subpte, *pte = kvm_pagetable;
	int val, *writable, *readonly, *cacher1, *cacher2;

	if (!(kvm_rdmsr(MSR_EFER) & EFER_LMA))
		tst_brk(TBROK, "Bootstrap did not enable 64bit paging");

	/*
	 * Find the first page table entry which branches. This entry was
	 * configured by bootstrap as follows:
	 * 0x00000000 - 0x3fffffff in pte[0] (identity mapped)
	 * 0x40000000 - 0x7fffffff in pte[1] (identity mapped)
	 * 0x80000000 - 0xbfffffff in pte[2] (unmapped)
	 * 0xc0000000 - 0xffffffff in pte[3] (only last page identity mapped)
	 */
	while (!pte[1].present) {
		tmp = kvm_get_page_address_pae(pte);
		pte = (struct page_table_entry_pae *)tmp;
	}

	/*
	 * Setup mapping above the 32bit address space. The test needs two
	 * different unused 1GB chunks of address space. Remapping part of
	 * the lower 4GB address space would make it harder to reproduce
	 * the bug because any memory access in the same 1GB chunk (even
	 * fetching instructions by the CPU) could evict entries from page
	 * table cache and force the bypassable write permission check
	 * to happen even on buggy kernels.
	 *
	 * Allocate 3 pages for page table + 2 pages for data
	 */
	writable = tst_heap_alloc_aligned(5 * PAGESIZE, PAGESIZE);
	memset(writable, 0, 5 * PAGESIZE);
	tmp = (uintptr_t)writable;
	pte[4].address = tmp >> 12;
	pte[4].user_access = 1;
	pte[4].writable = 1;
	pte[4].present = 1;
	pte[5] = pte[4];
	pte[5].writable = 0;

	subpte = (struct page_table_entry_pae *)tmp;
	subpte[0].address = (tmp + PAGESIZE) >> 12;
	subpte[0].user_access = 1;
	subpte[0].writable = 0;
	subpte[0].present = 1;
	subpte[1].address = (tmp + 2 * PAGESIZE) >> 12;
	subpte[1].user_access = 1;
	subpte[1].writable = 1;
	subpte[1].present = 1;

	subpte = (struct page_table_entry_pae *)(tmp + PAGESIZE);
	subpte[0].address = (tmp + 3 * PAGESIZE) >> 12;
	subpte[0].user_access = 1;
	subpte[0].writable = 1;
	subpte[0].present = 1;

	subpte = (struct page_table_entry_pae *)(tmp + 2 * PAGESIZE);
	subpte[0].address = (tmp + 4 * PAGESIZE) >> 12;
	subpte[0].user_access = 1;
	subpte[0].writable = 1;
	subpte[0].present = 1;

	/* Create pointers into the new mapping */
	cacher1 = (int *)0x100000000ULL;
	writable = (int *)0x100200000ULL;
	cacher2 = (int *)0x140000000ULL;
	readonly = (int *)0x140200000ULL;
	tst_set_interrupt_callback(INTR_PAGE_FAULT, handle_page_fault,
		readonly);

	/* Fill page table cache */
	val = *cacher1;
	*writable = val;
	val = *cacher2;

	/* Trigger page fault (unless the kernel is vulnerable) */
	*readonly = val;

	/* This line should be unreachable */
	tst_res(TFAIL, "Write to read-only address did not page fault");
}

#else /* __x86_64__ */
TST_TEST_TCONF("Test supported only on x86_64");
#endif /* __x86_64__ */

#else /* COMPILE_PAYLOAD */

#include "tst_module.h"

#define TDP_MMU_SYSFILE "/sys/module/kvm/parameters/tdp_mmu"
#define TDP_AMD_SYSFILE "/sys/module/kvm_amd/parameters/npt"
#define TDP_INTEL_SYSFILE "/sys/module/kvm_intel/parameters/ept"

static void disable_tdp(void)
{
	if (tst_read_bool_sys_param(TDP_AMD_SYSFILE) > 0)
		tst_module_reload("kvm_amd", (char *const[]){"npt=0", NULL});

	if (tst_read_bool_sys_param(TDP_INTEL_SYSFILE) > 0)
		tst_module_reload("kvm_intel", (char *const[]){"ept=0", NULL});

	if (tst_read_bool_sys_param(TDP_MMU_SYSFILE) > 0)
		tst_res(TINFO, "WARNING: tdp_mmu is enabled, beware of false negatives");
}

static void setup(void)
{
	disable_tdp();
	tst_kvm_setup();
}

static struct tst_test test = {
	.test_all = tst_kvm_run,
	.setup = setup,
	.cleanup = tst_kvm_cleanup,
	.needs_root = 1,
	.supported_archs = (const char *const []) {
		"x86_64",
		NULL
	},
	.tags = (struct tst_tag[]){
		{"linux-git", "b1bd5cba3306"},
		{"CVE", "2021-38198"},
		{}
	}
};

#endif /* COMPILE_PAYLOAD */
