/*
 *  prepare to run common code
 *
 *  Copyright (C) 2000 Andrea Arcangeli <andrea@suse.de> SuSE
 */

#include <linux/init.h>
#include <linux/linkage.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/percpu.h>
#include <linux/start_kernel.h>
#include <linux/io.h>
#include <linux/memblock.h>

#include <asm/processor.h>
#include <asm/proto.h>
#include <asm/smp.h>
#include <asm/setup.h>
#include <asm/desc.h>
#include <asm/pgtable.h>
#include <asm/tlbflush.h>
#include <asm/sections.h>
#include <asm/kdebug.h>
#include <asm/e820.h>
#include <asm/bios_ebda.h>

/* Create a new PMD entry */
int __init early_make_pgtable(unsigned long address)
{
	unsigned long physaddr = address - __PAGE_OFFSET;
	unsigned long i;
	pgdval_t pgd, *pgd_p;
	pudval_t pud, *pud_p;
	pmdval_t pmd, *pmd_p;

	if (address < __PAGE_OFFSET || physaddr >= MAXMEM)
		return -1;	/* Invalid address - puke */

	pgd_p = &init_level4_pgt[pgd_index(address)].pgd;
	pgd = *pgd_p;

	/*
	 * The use of __START_KERNEL_map rather than __PAGE_OFFSET here is
	 * critical -- __PAGE_OFFSET would point us back into the dynamic
	 * range and we might end up looping forever...
	 */
	if (pgd)
		pud_p = (pudval_t *)((pgd & PTE_PFN_MASK) + __START_KERNEL_map - phys_base);
	else {
		if ((char *)(_brk_end + PAGE_SIZE) > __brk_limit)
			return -1;
		pud_p = (pudval_t *)_brk_end;
		_brk_end += PAGE_SIZE;

		for (i = 0; i < PTRS_PER_PUD; i++)
			pud_p[i] = 0;
		*pgd_p = (pgdval_t)pud_p - __START_KERNEL_map + phys_base + _KERNPG_TABLE;
	}
	pud_p += pud_index(address);
	pud = *pud_p;

	if (pud)
		pmd_p = (pmdval_t *)((pud & PTE_PFN_MASK) + __START_KERNEL_map - phys_base);
	else {
		if ((char *)(_brk_end + PAGE_SIZE) > __brk_limit)
			return -1;
		pmd_p = (pmdval_t *)_brk_end;
		_brk_end += PAGE_SIZE;

		for (i = 0; i < PTRS_PER_PMD; i++)
			pmd_p[i] = 0;
		*pud_p = (pudval_t)pmd_p - __START_KERNEL_map + phys_base + _KERNPG_TABLE;
	}
	pmd = (physaddr & PMD_MASK) + __PAGE_KERNEL_LARGE;
	pmd_p[pmd_index(address)] = pmd;

	return 0;
}


static void __init zap_identity_mappings(void)
{
	pgd_t *pgd = pgd_offset_k(0UL);
	unsigned long pa_text = __pa_symbol(_text);
	unsigned long pa_end = __pa_symbol(_end);

	pgd_clear(pgd);

	/* When kernel is loaded above 512G */
	if (pa_text >= PGDIR_SIZE)
		pgd_clear(pgd + pgd_index(pa_text));
	if (pa_end - 1 >= PGDIR_SIZE)
		pgd_clear(pgd + pgd_index(pa_end - 1));

	__flush_tlb_all();
}

/* Don't add a printk in there. printk relies on the PDA which is not initialized 
   yet. */
static void __init clear_bss(void)
{
	memset(__bss_start, 0,
	       (unsigned long) __bss_stop - (unsigned long) __bss_start);
}

static unsigned long get_cmd_line_ptr(void)
{
	unsigned long cmd_line_ptr = boot_params.hdr.cmd_line_ptr;

	cmd_line_ptr |= (u64)boot_params.ext_cmd_line_ptr << 32;

	return cmd_line_ptr;
}

static void __init copy_bootdata(char *real_mode_data)
{
	char * command_line;
	unsigned long cmd_line_ptr;
	char *p;

	/*
	 * for 64bit bootloader path, those data could be above 4G,
	 * and we do not set ident mapping for them in head_64.S.
	 * So need to use ioremap to access them.
	 */
	p = early_memremap((unsigned long)real_mode_data, sizeof(boot_params));
	memcpy(&boot_params, p, sizeof(boot_params));
	early_iounmap(p, sizeof(boot_params));
	cmd_line_ptr = get_cmd_line_ptr();
	if (cmd_line_ptr) {
		command_line = early_memremap(cmd_line_ptr, COMMAND_LINE_SIZE);
		memcpy(boot_command_line, command_line, COMMAND_LINE_SIZE);
		early_iounmap(command_line, COMMAND_LINE_SIZE);
	}
}

void __init x86_64_start_kernel(char * real_mode_data)
{
	int i;

	/*
	 * Build-time sanity checks on the kernel image and module
	 * area mappings. (these are purely build-time and produce no code)
	 */
	BUILD_BUG_ON(MODULES_VADDR < KERNEL_IMAGE_START);
	BUILD_BUG_ON(MODULES_VADDR-KERNEL_IMAGE_START < KERNEL_IMAGE_SIZE);
	BUILD_BUG_ON(MODULES_LEN + KERNEL_IMAGE_SIZE > 2*PUD_SIZE);
	BUILD_BUG_ON((KERNEL_IMAGE_START & ~PMD_MASK) != 0);
	BUILD_BUG_ON((MODULES_VADDR & ~PMD_MASK) != 0);
	BUILD_BUG_ON(!(MODULES_VADDR > __START_KERNEL));
	BUILD_BUG_ON(!(((MODULES_END - 1) & PGDIR_MASK) ==
				(__START_KERNEL & PGDIR_MASK)));
	BUILD_BUG_ON(__fix_to_virt(__end_of_fixed_addresses) <= MODULES_END);

	/* clear bss before set_intr_gate with early_idt_handler */
	clear_bss();

	/* boot_params is in bss */
	early_ioremap_init();
	copy_bootdata(real_mode_data);

	/* Make NULL pointers segfault */
	zap_identity_mappings();

	for (i = 0; i < NUM_EXCEPTION_VECTORS; i++) {
#ifdef CONFIG_EARLY_PRINTK
		set_intr_gate(i, &early_idt_handlers[i]);
#else
		set_intr_gate(i, early_idt_handler);
#endif
	}
	load_idt((const struct desc_ptr *)&idt_descr);

	if (console_loglevel == 10)
		early_printk("Kernel alive\n");

	x86_64_start_reservations(real_mode_data);
}

void __init x86_64_start_reservations(char *real_mode_data)
{
	/*
	 * hdr.version is always not 0, so check it to see
	 *  if boot_params is copied or not.
	 */
	if (!boot_params.hdr.version) {
		early_ioremap_init();
		copy_bootdata(real_mode_data);
	}

	reserve_ebda_region();

	start_kernel();
}
