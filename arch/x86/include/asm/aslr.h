#ifndef _ASM_X86_ASLR_H

struct kaslr_setup_data {
	__u64 next;
	__u32 type;
	__u32 len;
	__u8 data[1];
};

#endif
