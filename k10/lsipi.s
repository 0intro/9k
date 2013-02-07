/*
 * Start-up request IPI handler.
 *
 * This code is executed on an application processor in response to receiving
 * a Start-up IPI (SIPI) from another processor.
 * This must be placed on a 4KiB boundary
 * somewhere in the 1st MiB of conventional memory. However,
 * due to some shortcuts below it's restricted further to within the 1st 64KiB.
 * The AP starts in real-mode, with
 *   CS selector set to the startup memory address/16;
 *   CS base set to startup memory address;
 *   CS limit set to 64KiB;
 *   CPL and IP set to 0.
 */
#include "mem.h"
#include "amd64l.h"

/*
 * Some machine instructions not handled well by [68][al].
 * This is a messy piece of code, requiring instructions in real mode,
 * protected mode (+long mode on amd64). The MODE psuedo-op of 6[al] handles
 * the latter two OK, but 'MODE $16' is incomplete, e.g. it does
 * not truncate operands appropriately, hence the ugly 'rMOVAX' macro.
 * Fortunately, the only other instruction executed in real mode that
 * could cause a problem (ORL) is encoded such that it will work OK.
 */
#define	DELAY		BYTE $0xeb;		/* JMP .+2 */		\
			BYTE $0x00
#define NOP		BYTE $0x90		/* NOP */

#define pFARJMP32(s, o)	BYTE $0xea;		/* far jmp ptr32:16 */	\
			LONG $o; WORD $s

#define rFARJMP16(s, o)	BYTE $0xea;		/* far jump ptr16:16 */	\
			WORD $o; WORD $s;
#define rFARJMP32(s, o)	BYTE $0x66;		/* far jump ptr32:16 */	\
			pFARJMP32(s, o)
#define rLGDT(gdtptr)	BYTE $0x0f;		/* LGDT */		\
			BYTE $0x01; BYTE $0x16;				\
			WORD $gdtptr
#define rMOVAX(i)	BYTE $0xb8;		/* i -> AX */		\
			WORD $i;

/*
 * Real mode. Welcome to 1978.
 * Load a basic GDT, turn on protected mode and make
 * inter-segment jump to the protected mode code.
 */
MODE $16

TEXT _real<>(SB), 1, $-4
	rFARJMP16(0, _endofheader<>-KZERO(SB))	/*  */

_startofheader:
	NOP; NOP; NOP
	QUAD	$0xa5a5a5a5a5a5a5a5

TEXT _gdt32p<>(SB), 1, $-4
	QUAD	$0x0000000000000000		/* NULL descriptor */
	QUAD	$0x00cf9a000000ffff		/* CS */
	QUAD	$0x00cf92000000ffff		/* DS */
	QUAD	$0x0020980000000000		/* Long mode CS */

TEXT _gdtptr32p<>(SB), 1, $-4
	WORD	$(4*8-1)			/* includes long mode */
	LONG	$_gdt32p<>-KZERO(SB)

TEXT _gdt64<>(SB), 1, $-4
	QUAD	$0x0000000000000000		/* NULL descriptor */
	QUAD	$0x0020980000000000		/* CS */
	QUAD	$0x0000800000000000		/* DS */

TEXT _gdtptr64v<>(SB), 1, $-4
	WORD	$(3*8-1)
	QUAD	$_gdt64<>(SB)

TEXT _endofheader<>(SB), 1, $-4
	MOVW	CS, AX
	MOVW	AX, DS				/* initialise DS */

	rLGDT(_gdtptr32p<>-KZERO(SB))		/* load a basic gdt */

	MOVL	CR0, AX
	ORL	$Pe, AX
	MOVL	AX, CR0				/* turn on protected mode */
	DELAY					/* JMP .+2 */

	rMOVAX	(SSEL(SiDS, SsTIGDT|SsRPL0))	/*  */
	MOVW	AX, DS
	MOVW	AX, ES
	MOVW	AX, FS
	MOVW	AX, GS
	MOVW	AX, SS

	rFARJMP32(SSEL(SiCS, SsTIGDT|SsRPL0), _protected<>-KZERO(SB))

/*
 * Protected mode. Welcome to 1982.
 * Get the local APIC ID from the memory mapped APIC;
#ifdef UseOwnPageTables
 * load the PDB with the page table address, which is located
 * in the word immediately preceeding _real<>-KZERO(SB);
 * this is also the (physical) address of the top of stack;
#else
 * load the PML4 with the shared page table address;
#endif
 * make an identity map for the inter-segment jump below;
 * enable and activate long mode;
 * make an inter-segment jump to the long mode code.
 */
MODE $32

/*
 * Macros for accessing page table entries; must turn
 * the C-style array-index macros into a page table byte
 * offset.
 */
#define PML4O(v)	((PTLX((v), 3))<<3)
#define PDPO(v)		((PTLX((v), 2))<<3)
#define PDO(v)		((PTLX((v), 1))<<3)
#define PTO(v)		((PTLX((v), 0))<<3)

TEXT _protected<>(SB), 1, $-4
	MOVL	$0xfee00000, BP			/* apicbase */
	MOVL	0x20(BP), BP			/* Id */
	SHRL	$24, BP				/* becomes RARG later */

#ifdef UseOwnPageTables
	MOVL	$_real<>-KZERO(SB), AX
	MOVL	-4(AX), SI			/* page table PML4 */
#else
	MOVL	$(0x00100000+MACHSTKSZ), SI	/* page table PML4 */
#endif

	MOVL	SI, AX
	MOVL	AX, CR3				/* load the mmu */

	MOVL	PML4O(KZERO)(AX), DX		/* PML4E for KZERO, PMAPADDR */
	MOVL	DX, PML4O(0)(AX)		/* PML4E for identity map */

	ANDL	$~((1<<PTSHFT)-1), DX		/* lop off attribute bits */
	MOVL	DX, AX				/* PDP for KZERO */
	MOVL	PDPO(KZERO)(AX), DX		/* PDPE for KZERO, PMAPADDR */
	MOVL	DX, PDPO(0)(AX)			/* PDPE for identity map */

	ANDL	$~((1<<PTSHFT)-1), DX		/* lop off attribute bits */
	MOVL	DX, AX				/* PD for KZERO */

	MOVL	PDO(KZERO)(AX), DX		/* PDE for KZERO 0-2MiB */
	MOVL	DX, PDO(0)(AX)			/* PDE for identity 0-2MiB */

/*
 * Enable and activate Long Mode. From the manual:
 * 	make sure Page Size Extentions are off, and Page Global
 *	Extensions and Physical Address Extensions are on in CR4;
 *	set Long Mode Enable in the Extended Feature Enable MSR;
 *	set Paging Enable in CR0;
 *	make an inter-segment jump to the Long Mode code.
 * It's all in 32-bit mode until the jump is made.
 */
TEXT _lme<>(SB), 1, $-4
	MOVL	CR4, AX
	ANDL	$~Pse, AX			/* Page Size */
	ORL	$(Pge|Pae), AX			/* Page Global, Phys. Address */
	MOVL	AX, CR4

	MOVL	$Efer, CX			/* Extended Feature Enable */
	RDMSR
	ORL	$Lme, AX			/* Long Mode Enable */
	WRMSR

	MOVL	CR0, DX
	ANDL	$~(Cd|Nw|Ts|Mp), DX
	ORL	$(Pg|Wp), DX			/* Paging Enable */
	MOVL	DX, CR0

	pFARJMP32(SSEL(3, SsTIGDT|SsRPL0), _identity<>-KZERO(SB))

/*
 * Long mode. Welcome to 2003.
 * Jump out of the identity map space;
 * load a proper long mode GDT;
 * zap the identity map;
 * initialise the stack and call the
 * C startup code in m->splpc.
 */
MODE $64

TEXT _identity<>(SB), 1, $-4
	MOVQ	$_start64v<>(SB), AX
	JMP*	AX

TEXT _start64v<>(SB), 1, $-4
	MOVQ	$_gdtptr64v<>(SB), AX
	MOVL	(AX), GDTR

	XORQ	DX, DX
	MOVW	DX, DS				/* not used in long mode */
	MOVW	DX, ES				/* not used in long mode */
	MOVW	DX, FS
	MOVW	DX, GS
	MOVW	DX, SS				/* not used in long mode */

	MOVLQZX	SI, SI				/* PML4-KZERO */
	MOVQ	SI, AX
	ADDQ	$KZERO, AX			/* PML4 and top of stack */

	MOVQ	AX, SP				/* set stack */

	MOVQ	PML4O(0)(AX), BX		/* PDPE identity map physical */
	ANDQ	$~((1<<PTSHFT)-1), BX		/* lop off attribute bits */
	ADDQ	$KZERO, BX			/* PDP identity map virtual */
	MOVQ	DX, PML4O(0)(AX)		/* zap identity map PML4E */

	MOVQ	PDPO(0)(BX), AX			/* PDE identity map physical */
	ANDQ	$~((1<<PTSHFT)-1), AX		/* lop off attribute bits */
	ADDQ	$KZERO, AX			/* PD identity map virtual */
	MOVQ	DX, PDPO(0)(BX)			/* zap identity map PDPE */

	MOVQ	DX, PDO(0)(AX)			/* zap identity map PDE */

	MOVQ	SI, CR3				/* flush TLB */
#ifndef UseOwnPageTables
	/*
	 * SI still points to the base of the bootstrap
	 * processor page tables.
	 * Want to use that for clearing the identity map,
	 * but want to use the passed-in address for
	 * setting up the stack and Mach.
	 */
	MOVQ	$_real<>(SB), AX
	MOVL	-4(AX), SI			/* PML4 */
	MOVLQZX	SI, SI				/* PML4-KZERO */
#endif
	MOVQ	SI, AX
	ADDQ	$KZERO, AX			/* PML4 and top of stack */

	MOVQ	AX, SP				/* set stack */

	ADDQ	$(4*PTSZ+PGSZ), AX		/* PML4+PDP+PD+PT+vsvm */
	MOVQ	AX, RMACH			/* Mach */
	MOVQ	DX, RUSER

	PUSHQ	DX				/* clear flags */
	POPFQ

	MOVLQZX	RARG, RARG			/* APIC ID */
	PUSHQ	RARG				/* apicno */

	MOVQ	8(RMACH), AX			/* m->splpc */
	CALL*	AX				/* CALL squidboy(SB) */

_ndnr:
	JMP	_ndnr
