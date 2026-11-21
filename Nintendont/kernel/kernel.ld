OUTPUT_FORMAT("elf32-bigarm")
OUTPUT_ARCH(arm)
EXTERN(_start)
ENTRY(_start)

__stack_size = 0x2000;
__phy_stack_addr = 0x12FF0000 + __stack_size;

__di_stack_size = 0x1000;
__di_stack_addr = __phy_stack_addr + __di_stack_size;

__realdi_stack_size = 0x400;
__realdi_stack_addr = __di_stack_addr + __realdi_stack_size;

__hid_read_stack_size = 0x400;
__hid_read_stack_addr = __realdi_stack_addr + __hid_read_stack_size;

__hid_change_stack_size = 0x400;
__hid_change_stack_addr = __hid_read_stack_addr + __hid_change_stack_size;

__intr_stack_size = 0x400;
__intr_stack_addr = __hid_change_stack_addr + __intr_stack_size;

__blk_stack_size = 0x400;
__blk_stack_addr = __intr_stack_addr + __blk_stack_size;

__slippi_network_stack_size = 0x2000;
__slippi_network_stack_addr = __blk_stack_addr + __slippi_network_stack_size;

__slippi_stack_size = 0x2000;
__slippi_stack_addr = __slippi_network_stack_addr + __slippi_stack_size;

__slippi_network_broadcast_stack_size = 0x400;
__slippi_network_broadcast_stack_addr = __slippi_stack_addr + __slippi_network_broadcast_stack_size;

__slippi_debug_stack_size = 0x400;
__slippi_debug_stack_addr = __slippi_network_broadcast_stack_addr + __slippi_debug_stack_size;

__ven_change_stack_size = 0x400;
__ven_change_stack_addr = __slippi_debug_stack_addr + __ven_change_stack_size;

/* Slippi Online (slippi_online/): network, receive, send completion and
 * self-test and diagnostics threads */
__slippi_online_stack_size = 0x2000;
__slippi_online_stack_addr = __ven_change_stack_addr + __slippi_online_stack_size;

__slippi_online_rx_stack_size = 0x1000;
__slippi_online_rx_stack_addr = __slippi_online_stack_addr + __slippi_online_rx_stack_size;

__slippi_online_done_stack_size = 0x800;
__slippi_online_done_stack_addr = __slippi_online_rx_stack_addr + __slippi_online_done_stack_size;

__slippi_online_test_stack_size = 0x1000;
__slippi_online_test_stack_addr = __slippi_online_done_stack_addr + __slippi_online_test_stack_size;

__slippi_online_diag_stack_size = 0x1800;	/* (the problem reports: the card's writes, FatFS, are made on it) */
__slippi_online_diag_stack_addr = __slippi_online_test_stack_addr + __slippi_online_diag_stack_size;

__slippi_online_stack_size = 0x2000;
__slippi_online_stack_addr = __ven_change_stack_addr + __slippi_online_stack_size;

__slippi_online_rx_stack_size = 0x1000;
__slippi_online_rx_stack_addr = __slippi_online_stack_addr + __slippi_online_rx_stack_size;

__slippi_online_done_stack_size = 0x800;
__slippi_online_done_stack_addr = __slippi_online_rx_stack_addr + __slippi_online_done_stack_size;

MEMORY {

	/* SlippWii: the megabyte the loader keeps for the kernel ends at 0x13000000, and its last
	 * 64 KB lay unused (but for kernelboot's stub at 0x12FFFE00 and the words after it). The data
	 * and the stacks now sit 64 KB higher and the code region has that much more: the TLS client
	 * of Slippi's matchmaking (so_tls.c, BearSSL) needs about 50 KB. */
	code		: ORIGIN = 0x12F00000,	LENGTH = 0x70000
	data		: ORIGIN = 0x12F70000,	LENGTH = 0x80000
	stack		: ORIGIN = 0x12FF0000,	LENGTH = 0xF000
}

PHDRS
{
	code  PT_LOAD AT ( 0x12F00000 );
	data  PT_LOAD AT ( 0x12F70000 );
	stack PT_LOAD AT ( 0x12FF0000 );
}

SECTIONS
{
	.init (0x12F00000) : AT( 0x12F00000 )
	{
		*(.init)
		. = ALIGN(4);
	} >code :code

	.text (0x12F00000 + SIZEOF(.init)) : AT( 0x12F00000 + SIZEOF(.init) )
	{
		*(.text*)
		*(.text.*)
		*(.gnu.warning)
		*(.gnu.linkonce.t*)
		*(.glue_7)
		*(.glue_7t)
		/* Slippi Online's boot codes (gecko/g_online_boot.h, read-only):
		 * here, where there is room, not in the full data region */
		*(.rodata.g_online_boot)
		/* and the TLS client's tables and the matchmaking service's texts (read-only too) */
		*libbearssl-arm.a:*(.rodata .rodata.*)
		*so_tls.o(.rodata .rodata.*)
		*so_ws.o(.rodata .rodata.*)
		*so_ws_frame.o(.rodata .rodata.*)
		*so_mm_ws.o(.rodata .rodata.*)
		*so_tcp.o(.rodata .rodata.*)
		*so_stun.o(.rodata .rodata.*)
		*so_stun_io.o(.rodata .rodata.*)
		. = ALIGN(4);
	} >code :code

	.rodata (0x12F70000) : AT( 0x12F70000 )
	{
		*(.rodata)
		*all.rodata*(*)
		*(.roda)
		*(.rodata.*)
		*(.gnu.linkonce.r*)
		. = ALIGN(4);
	} >data :data

	.data (0x12F70000 + SIZEOF(.rodata)) : AT( 0x12F70000 + SIZEOF(.rodata) )
	{
		*(.data)
		*(.data.*)
		*(.gnu.linkonce.d*)
		. = ALIGN(4);
	} >data :data

	.bss (0x12F70000 + SIZEOF(.rodata) + SIZEOF(.data)) : AT( 0x12F70000 + SIZEOF(.rodata) + SIZEOF(.data) )
	{
		__bss_start = . ;
		*(.dynbss)
		*(.gnu.linkonce.b*)
		*(.bss*)
		*(COMMON)
		. = ALIGN(4);
		__bss_end = . ;
	} >data :data

	.stack (0x12FF0000) : AT( 0x12FF0000 )
	{
		__stack_end = .;
		. = . +__stack_size;
		. = ALIGN(64);
		__stack_addr = .;
	} >stack :stack

}

