// SPDX-License-Identifier: GPL-2.0
/*
 * Procedures for drawing on the screen early on in the boot process.
 *
 * Benjamin Herrenschmidt <benh@kernel.crashing.org>
 */
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/memblock.h>

#include <asm/sections.h>
#include <asm/prom.h>
#include <asm/btext.h>
#include <asm/page.h>
#include <asm/mmu.h>
#include <asm/pgtable.h>
#include <asm/io.h>
#include <asm/processor.h>
#include <asm/udbg.h>

#define NO_SCROLL

#ifndef NO_SCROLL
static void scrollscreen(void);
#endif

#define __force_data __attribute__((__section__(".data")))

static int g_loc_X __force_data;
static int g_loc_Y __force_data;
static int g_max_loc_X __force_data;
static int g_max_loc_Y __force_data;

static int dispDeviceRowBytes __force_data;
static int dispDeviceDepth  __force_data;
static int dispDeviceRect[4] __force_data;
static unsigned char *dispDeviceBase __force_data;
static unsigned char *logicalDisplayBase __force_data;

unsigned long disp_BAT[2] __initdata = {0, 0};

#define cmapsz	(16*256)

static unsigned char vga_font[cmapsz];

int boot_text_mapped __force_data = 0;
int force_printk_to_btext = 0;

extern void rmci_on(void);
extern void rmci_off(void);

static inline void rmci_maybe_on(void)
{
#if defined(CONFIG_PPC_EARLY_DEBUG_BOOTX) && defined(CONFIG_PPC64)
	if (!(mfmsr() & MSR_DR))
		rmci_on();
#endif
}

static inline void rmci_maybe_off(void)
{
#if defined(CONFIG_PPC_EARLY_DEBUG_BOOTX) && defined(CONFIG_PPC64)
	if (!(mfmsr() & MSR_DR))
		rmci_off();
#endif
}


#ifdef CONFIG_PPC32
/* Calc BAT values for mapping the display and store them
 * in disp_BAT.  Those values are then used from head.S to map
 * the display during identify_machine() and MMU_Init()
 *
 * The display is mapped to virtual address 0xD0000000, rather
 * than 1:1, because some some CHRP machines put the frame buffer
 * in the region starting at 0xC0000000 (PAGE_OFFSET).
 * This mapping is temporary and will disappear as soon as the
 * setup done by MMU_Init() is applied.
 *
 * For now, we align the BAT and then map 8Mb on 601 and 16Mb
 * on other PPCs. This may cause trouble if the framebuffer
 * is really badly aligned, but I didn't encounter this case
 * yet.
 */
void __init btext_prepare_BAT(void)
{
	unsigned long vaddr = PAGE_OFFSET + 0x10000000;
	unsigned long addr;
	unsigned long lowbits;

	addr = (unsigned long)dispDeviceBase;
	if (!addr) {
		boot_text_mapped = 0;
		return;
	}
	if (PVR_VER(mfspr(SPRN_PVR)) != 1) {
		/* 603, 604, G3, G4, ... */
		lowbits = addr & ~0xFF000000UL;
		addr &= 0xFF000000UL;
		disp_BAT[0] = vaddr | (BL_16M<<2) | 2;
		disp_BAT[1] = addr | (_PAGE_NO_CACHE | _PAGE_GUARDED | BPP_RW);	
	} else {
		/* 601 */
		lowbits = addr & ~0xFF800000UL;
		addr &= 0xFF800000UL;
		disp_BAT[0] = vaddr | (_PAGE_NO_CACHE | PP_RWXX) | 4;
		disp_BAT[1] = addr | BL_8M | 0x40;
	}
	logicalDisplayBase = (void *) (vaddr + lowbits);
}
#endif


/* This function can be used to enable the early boot text when doing
 * OF booting or within bootx init. It must be followed by a btext_unmap()
 * call before the logical address becomes unusable
 */
void __init btext_setup_display(int width, int height, int depth, int pitch,
				unsigned long address)
{
	g_loc_X = 0;
	g_loc_Y = 0;
	g_max_loc_X = width / 8;
	g_max_loc_Y = height / 16;
	logicalDisplayBase = (unsigned char *)address;
	dispDeviceBase = (unsigned char *)address;
	dispDeviceRowBytes = pitch;
	dispDeviceDepth = depth == 15 ? 16 : depth;
	dispDeviceRect[0] = dispDeviceRect[1] = 0;
	dispDeviceRect[2] = width;
	dispDeviceRect[3] = height;
	boot_text_mapped = 1;
}

void __init btext_unmap(void)
{
	boot_text_mapped = 0;
}

/* Here's a small text engine to use during early boot
 * or for debugging purposes
 *
 * todo:
 *
 *  - build some kind of vgacon with it to enable early printk
 *  - move to a separate file
 *  - add a few video driver hooks to keep in sync with display
 *    changes.
 */

void btext_map(void)
{
	unsigned long base, offset, size;
	unsigned char *vbase;

	/* By default, we are no longer mapped */
	boot_text_mapped = 0;
	if (!dispDeviceBase)
		return;
	base = ((unsigned long) dispDeviceBase) & 0xFFFFF000UL;
	offset = ((unsigned long) dispDeviceBase) - base;
	size = dispDeviceRowBytes * dispDeviceRect[3] + offset
		+ dispDeviceRect[0];
	vbase = ioremap_wc(base, size);
	if (!vbase)
		return;
	logicalDisplayBase = vbase + offset;
	boot_text_mapped = 1;
}

static int btext_initialize(struct device_node *np)
{
	unsigned int width, height, depth, pitch;
	unsigned long address = 0;
	const u32 *prop;

	prop = of_get_property(np, "linux,bootx-width", NULL);
	if (prop == NULL)
		prop = of_get_property(np, "width", NULL);
	if (prop == NULL)
		return -EINVAL;
	width = *prop;
	prop = of_get_property(np, "linux,bootx-height", NULL);
	if (prop == NULL)
		prop = of_get_property(np, "height", NULL);
	if (prop == NULL)
		return -EINVAL;
	height = *prop;
	prop = of_get_property(np, "linux,bootx-depth", NULL);
	if (prop == NULL)
		prop = of_get_property(np, "depth", NULL);
	if (prop == NULL)
		return -EINVAL;
	depth = *prop;
	pitch = width * ((depth + 7) / 8);
	prop = of_get_property(np, "linux,bootx-linebytes", NULL);
	if (prop == NULL)
		prop = of_get_property(np, "linebytes", NULL);
	if (prop && *prop != 0xffffffffu)
		pitch = *prop;
	if (pitch == 1)
		pitch = 0x1000;
	prop = of_get_property(np, "linux,bootx-addr", NULL);
	if (prop == NULL)
		prop = of_get_property(np, "address", NULL);
	if (prop)
		address = *prop;

	/* FIXME: Add support for PCI reg properties. Right now, only
	 * reliable on macs
	 */
	if (address == 0)
		return -EINVAL;

	g_loc_X = 0;
	g_loc_Y = 0;
	g_max_loc_X = width / 8;
	g_max_loc_Y = height / 16;
	dispDeviceBase = (unsigned char *)address;
	dispDeviceRowBytes = pitch;
	dispDeviceDepth = depth == 15 ? 16 : depth;
	dispDeviceRect[0] = dispDeviceRect[1] = 0;
	dispDeviceRect[2] = width;
	dispDeviceRect[3] = height;

	btext_map();

	return 0;
}

int __init btext_find_display(int allow_nonstdout)
{
	struct device_node *np = of_stdout;
	int rc = -ENODEV;

	if (!of_node_is_type(np, "display")) {
		printk("boot stdout isn't a display !\n");
		np = NULL;
	}
	if (np)
		rc = btext_initialize(np);
	if (rc == 0 || !allow_nonstdout)
		return rc;

	for_each_node_by_type(np, "display") {
		if (of_get_property(np, "linux,opened", NULL)) {
			printk("trying %pOF ...\n", np);
			rc = btext_initialize(np);
			printk("result: %d\n", rc);
		}
		if (rc == 0)
			break;
	}
	return rc;
}

/* Calc the base address of a given point (x,y) */
static unsigned char * calc_base(int x, int y)
{
	unsigned char *base;

	base = logicalDisplayBase;
	if (!base)
		base = dispDeviceBase;
	base += (x + dispDeviceRect[0]) * (dispDeviceDepth >> 3);
	base += (y + dispDeviceRect[1]) * dispDeviceRowBytes;
	return base;
}

/* Adjust the display to a new resolution */
void btext_update_display(unsigned long phys, int width, int height,
			  int depth, int pitch)
{
	if (!dispDeviceBase)
		return;

	/* check it's the same frame buffer (within 256MB) */
	if ((phys ^ (unsigned long)dispDeviceBase) & 0xf0000000)
		return;

	dispDeviceBase = (__u8 *) phys;
	dispDeviceRect[0] = 0;
	dispDeviceRect[1] = 0;
	dispDeviceRect[2] = width;
	dispDeviceRect[3] = height;
	dispDeviceDepth = depth;
	dispDeviceRowBytes = pitch;
	if (boot_text_mapped) {
		iounmap(logicalDisplayBase);
		boot_text_mapped = 0;
	}
	btext_map();
	g_loc_X = 0;
	g_loc_Y = 0;
	g_max_loc_X = width / 8;
	g_max_loc_Y = height / 16;
}
EXPORT_SYMBOL(btext_update_display);

void btext_clearscreen(void)
{
	unsigned int *base	= (unsigned int *)calc_base(0, 0);
	unsigned long width 	= ((dispDeviceRect[2] - dispDeviceRect[0]) *
					(dispDeviceDepth >> 3)) >> 2;
	int i,j;

	rmci_maybe_on();
	for (i=0; i<(dispDeviceRect[3] - dispDeviceRect[1]); i++)
	{
		unsigned int *ptr = base;
		for(j=width; j; --j)
			*(ptr++) = 0;
		base += (dispDeviceRowBytes >> 2);
	}
	rmci_maybe_off();
}

void btext_flushscreen(void)
{
	unsigned int *base	= (unsigned int *)calc_base(0, 0);
	unsigned long width 	= ((dispDeviceRect[2] - dispDeviceRect[0]) *
					(dispDeviceDepth >> 3)) >> 2;
	int i,j;

	for (i=0; i < (dispDeviceRect[3] - dispDeviceRect[1]); i++)
	{
		unsigned int *ptr = base;
		for(j = width; j > 0; j -= 8) {
			__asm__ __volatile__ ("dcbst 0,%0" :: "r" (ptr));
			ptr += 8;
		}
		base += (dispDeviceRowBytes >> 2);
	}
	__asm__ __volatile__ ("sync" ::: "memory");
}

void btext_flushline(void)
{
	unsigned int *base	= (unsigned int *)calc_base(0, g_loc_Y << 4);
	unsigned long width 	= ((dispDeviceRect[2] - dispDeviceRect[0]) *
					(dispDeviceDepth >> 3)) >> 2;
	int i,j;

	for (i=0; i < 16; i++)
	{
		unsigned int *ptr = base;
		for(j = width; j > 0; j -= 8) {
			__asm__ __volatile__ ("dcbst 0,%0" :: "r" (ptr));
			ptr += 8;
		}
		base += (dispDeviceRowBytes >> 2);
	}
	__asm__ __volatile__ ("sync" ::: "memory");
}


#ifndef NO_SCROLL
static void scrollscreen(void)
{
	unsigned int *src     	= (unsigned int *)calc_base(0,16);
	unsigned int *dst     	= (unsigned int *)calc_base(0,0);
	unsigned long width    	= ((dispDeviceRect[2] - dispDeviceRect[0]) *
				   (dispDeviceDepth >> 3)) >> 2;
	int i,j;

	rmci_maybe_on();

	for (i=0; i<(dispDeviceRect[3] - dispDeviceRect[1] - 16); i++)
	{
		unsigned int *src_ptr = src;
		unsigned int *dst_ptr = dst;
		for(j=width; j; --j)
			*(dst_ptr++) = *(src_ptr++);
		src += (dispDeviceRowBytes >> 2);
		dst += (dispDeviceRowBytes >> 2);
	}
	for (i=0; i<16; i++)
	{
		unsigned int *dst_ptr = dst;
		for(j=width; j; --j)
			*(dst_ptr++) = 0;
		dst += (dispDeviceRowBytes >> 2);
	}

	rmci_maybe_off();
}
#endif /* ndef NO_SCROLL */

static unsigned int expand_bits_8[16] = {
	0x00000000,
	0x000000ff,
	0x0000ff00,
	0x0000ffff,
	0x00ff0000,
	0x00ff00ff,
	0x00ffff00,
	0x00ffffff,
	0xff000000,
	0xff0000ff,
	0xff00ff00,
	0xff00ffff,
	0xffff0000,
	0xffff00ff,
	0xffffff00,
	0xffffffff
};

static unsigned int expand_bits_16[4] = {
	0x00000000,
	0x0000ffff,
	0xffff0000,
	0xffffffff
};


static void draw_byte_32(unsigned char *font, unsigned int *base, int rb)
{
	int l, bits;
	int fg = 0xFFFFFFFFUL;
	int bg = 0x00000000UL;

	for (l = 0; l < 16; ++l)
	{
		bits = *font++;
		base[0] = (-(bits >> 7) & fg) ^ bg;
		base[1] = (-((bits >> 6) & 1) & fg) ^ bg;
		base[2] = (-((bits >> 5) & 1) & fg) ^ bg;
		base[3] = (-((bits >> 4) & 1) & fg) ^ bg;
		base[4] = (-((bits >> 3) & 1) & fg) ^ bg;
		base[5] = (-((bits >> 2) & 1) & fg) ^ bg;
		base[6] = (-((bits >> 1) & 1) & fg) ^ bg;
		base[7] = (-(bits & 1) & fg) ^ bg;
		base = (unsigned int *) ((char *)base + rb);
	}
}

static inline void draw_byte_16(unsigned char *font, unsigned int *base, int rb)
{
	int l, bits;
	int fg = 0xFFFFFFFFUL;
	int bg = 0x00000000UL;
	unsigned int *eb = (int *)expand_bits_16;

	for (l = 0; l < 16; ++l)
	{
		bits = *font++;
		base[0] = (eb[bits >> 6] & fg) ^ bg;
		base[1] = (eb[(bits >> 4) & 3] & fg) ^ bg;
		base[2] = (eb[(bits >> 2) & 3] & fg) ^ bg;
		base[3] = (eb[bits & 3] & fg) ^ bg;
		base = (unsigned int *) ((char *)base + rb);
	}
}

static inline void draw_byte_8(unsigned char *font, unsigned int *base, int rb)
{
	int l, bits;
	int fg = 0x0F0F0F0FUL;
	int bg = 0x00000000UL;
	unsigned int *eb = (int *)expand_bits_8;

	for (l = 0; l < 16; ++l)
	{
		bits = *font++;
		base[0] = (eb[bits >> 4] & fg) ^ bg;
		base[1] = (eb[bits & 0xf] & fg) ^ bg;
		base = (unsigned int *) ((char *)base + rb);
	}
}

static noinline void draw_byte(unsigned char c, long locX, long locY)
{
	unsigned char *base	= calc_base(locX << 3, locY << 4);
	unsigned char *font	= &vga_font[((unsigned int)c) * 16];
	int rb			= dispDeviceRowBytes;

	rmci_maybe_on();
	switch(dispDeviceDepth) {
	case 24:
	case 32:
		draw_byte_32(font, (unsigned int *)base, rb);
		break;
	case 15:
	case 16:
		draw_byte_16(font, (unsigned int *)base, rb);
		break;
	case 8:
		draw_byte_8(font, (unsigned int *)base, rb);
		break;
	}
	rmci_maybe_off();
}

void btext_drawchar(char c)
{
	int cline = 0;
#ifdef NO_SCROLL
	int x;
#endif
	if (!boot_text_mapped)
		return;

	switch (c) {
	case '\b':
		if (g_loc_X > 0)
			--g_loc_X;
		break;
	case '\t':
		g_loc_X = (g_loc_X & -8) + 8;
		break;
	case '\r':
		g_loc_X = 0;
		break;
	case '\n':
		g_loc_X = 0;
		g_loc_Y++;
		cline = 1;
		break;
	default:
		draw_byte(c, g_loc_X++, g_loc_Y);
	}
	if (g_loc_X >= g_max_loc_X) {
		g_loc_X = 0;
		g_loc_Y++;
		cline = 1;
	}
#ifndef NO_SCROLL
	while (g_loc_Y >= g_max_loc_Y) {
		scrollscreen();
		g_loc_Y--;
	}
#else
	/* wrap around from bottom to top of screen so we don't
	   waste time scrolling each line.  -- paulus. */
	if (g_loc_Y >= g_max_loc_Y)
		g_loc_Y = 0;
	if (cline) {
		for (x = 0; x < g_max_loc_X; ++x)
			draw_byte(' ', x, g_loc_Y);
	}
#endif
}

void btext_drawstring(const char *c)
{
	if (!boot_text_mapped)
		return;
	while (*c)
		btext_drawchar(*c++);
}

void btext_drawtext(const char *c, unsigned int len)
{
	if (!boot_text_mapped)
		return;
	while (len--)
		btext_drawchar(*c++);
}

void btext_drawhex(unsigned long v)
{
	if (!boot_text_mapped)
		return;
#ifdef CONFIG_PPC64
	btext_drawchar(hex_asc_hi(v >> 56));
	btext_drawchar(hex_asc_lo(v >> 56));
	btext_drawchar(hex_asc_hi(v >> 48));
	btext_drawchar(hex_asc_lo(v >> 48));
	btext_drawchar(hex_asc_hi(v >> 40));
	btext_drawchar(hex_asc_lo(v >> 40));
	btext_drawchar(hex_asc_hi(v >> 32));
	btext_drawchar(hex_asc_lo(v >> 32));
#endif
	btext_drawchar(hex_asc_hi(v >> 24));
	btext_drawchar(hex_asc_lo(v >> 24));
	btext_drawchar(hex_asc_hi(v >> 16));
	btext_drawchar(hex_asc_lo(v >> 16));
	btext_drawchar(hex_asc_hi(v >> 8));
	btext_drawchar(hex_asc_lo(v >> 8));
	btext_drawchar(hex_asc_hi(v));
	btext_drawchar(hex_asc_lo(v));
	btext_drawchar(' ');
}

void __init udbg_init_btext(void)
{
	/* If btext is enabled, we might have a BAT setup for early display,
	 * thus we do enable some very basic udbg output
	 */
	udbg_putc = btext_drawchar;
}

static unsigned char vga_font[cmapsz] = {
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0x81, 0xa5, 0x81, 0x81, 0xbd,
0x99, 0x81, 0x81, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0xff,
0xdb, 0xff, 0xff, 0xc3, 0xe7, 0xff, 0xff, 0x7e, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x6c, 0xfe, 0xfe, 0xfe, 0xfe, 0x7c, 0x38, 0x10,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x38, 0x7c, 0xfe,
0x7c, 0x38, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18,
0x3c, 0x3c, 0xe7, 0xe7, 0xe7, 0x18, 0x18, 0x3c, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x18, 0x3c, 0x7e, 0xff, 0xff, 0x7e, 0x18, 0x18, 0x3c,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x3c,
0x3c, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff,
0xff, 0xff, 0xe7, 0xc3, 0xc3, 0xe7, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x66, 0x42, 0x42, 0x66, 0x3c, 0x00,
0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xc3, 0x99, 0xbd,
0xbd, 0x99, 0xc3, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x1e, 0x0e,
0x1a, 0x32, 0x78, 0xcc, 0xcc, 0xcc, 0xcc, 0x78, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x3c, 0x66, 0x66, 0x66, 0x66, 0x3c, 0x18, 0x7e, 0x18, 0x18,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x33, 0x3f, 0x30, 0x30, 0x30,
0x30, 0x70, 0xf0, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f, 0x63,
0x7f, 0x63, 0x63, 0x63, 0x63, 0x67, 0xe7, 0xe6, 0xc0, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x18, 0x18, 0xdb, 0x3c, 0xe7, 0x3c, 0xdb, 0x18, 0x18,
0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xc0, 0xe0, 0xf0, 0xf8, 0xfe, 0xf8,
0xf0, 0xe0, 0xc0, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x06, 0x0e,
0x1e, 0x3e, 0xfe, 0x3e, 0x1e, 0x0e, 0x06, 0x02, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x18, 0x3c, 0x7e, 0x18, 0x18, 0x18, 0x7e, 0x3c, 0x18, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
0x66, 0x00, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7f, 0xdb,
0xdb, 0xdb, 0x7b, 0x1b, 0x1b, 0x1b, 0x1b, 0x1b, 0x00, 0x00, 0x00, 0x00,
0x00, 0x7c, 0xc6, 0x60, 0x38, 0x6c, 0xc6, 0xc6, 0x6c, 0x38, 0x0c, 0xc6,
0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0xfe, 0xfe, 0xfe, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x3c,
0x7e, 0x18, 0x18, 0x18, 0x7e, 0x3c, 0x18, 0x7e, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x18, 0x3c, 0x7e, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
0x18, 0x7e, 0x3c, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x18, 0x0c, 0xfe, 0x0c, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x60, 0xfe, 0x60, 0x30, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0xc0,
0xc0, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x24, 0x66, 0xff, 0x66, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x10, 0x38, 0x38, 0x7c, 0x7c, 0xfe, 0xfe, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0xfe, 0x7c, 0x7c,
0x38, 0x38, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x18, 0x3c, 0x3c, 0x3c, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18,
0x00, 0x00, 0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x24, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6c,
0x6c, 0xfe, 0x6c, 0x6c, 0x6c, 0xfe, 0x6c, 0x6c, 0x00, 0x00, 0x00, 0x00,
0x18, 0x18, 0x7c, 0xc6, 0xc2, 0xc0, 0x7c, 0x06, 0x06, 0x86, 0xc6, 0x7c,
0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc2, 0xc6, 0x0c, 0x18,
0x30, 0x60, 0xc6, 0x86, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x6c,
0x6c, 0x38, 0x76, 0xdc, 0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00,
0x00, 0x30, 0x30, 0x30, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x18, 0x30, 0x30, 0x30, 0x30,
0x30, 0x30, 0x18, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x18,
0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x66, 0x3c, 0xff, 0x3c, 0x66, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x7e,
0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0x30, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x02, 0x06, 0x0c, 0x18, 0x30, 0x60, 0xc0, 0x80, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x7c, 0xc6, 0xc6, 0xce, 0xde, 0xf6, 0xe6, 0xc6, 0xc6, 0x7c,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x38, 0x78, 0x18, 0x18, 0x18,
0x18, 0x18, 0x18, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0xc6,
0x06, 0x0c, 0x18, 0x30, 0x60, 0xc0, 0xc6, 0xfe, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x7c, 0xc6, 0x06, 0x06, 0x3c, 0x06, 0x06, 0x06, 0xc6, 0x7c,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x1c, 0x3c, 0x6c, 0xcc, 0xfe,
0x0c, 0x0c, 0x0c, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0xc0,
0xc0, 0xc0, 0xfc, 0x06, 0x06, 0x06, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x38, 0x60, 0xc0, 0xc0, 0xfc, 0xc6, 0xc6, 0xc6, 0xc6, 0x7c,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0xc6, 0x06, 0x06, 0x0c, 0x18,
0x30, 0x30, 0x30, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0xc6,
0xc6, 0xc6, 0x7c, 0xc6, 0xc6, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0x7e, 0x06, 0x06, 0x06, 0x0c, 0x78,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00,
0x00, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x18, 0x18, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x06, 0x0c, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0c, 0x06,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0x00, 0x00,
0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60,
0x30, 0x18, 0x0c, 0x06, 0x0c, 0x18, 0x30, 0x60, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x7c, 0xc6, 0xc6, 0x0c, 0x18, 0x18, 0x18, 0x00, 0x18, 0x18,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0xde, 0xde,
0xde, 0xdc, 0xc0, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x38,
0x6c, 0xc6, 0xc6, 0xfe, 0xc6, 0xc6, 0xc6, 0xc6, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0xfc, 0x66, 0x66, 0x66, 0x7c, 0x66, 0x66, 0x66, 0x66, 0xfc,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x66, 0xc2, 0xc0, 0xc0, 0xc0,
0xc0, 0xc2, 0x66, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x6c,
0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x6c, 0xf8, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0xfe, 0x66, 0x62, 0x68, 0x78, 0x68, 0x60, 0x62, 0x66, 0xfe,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0x66, 0x62, 0x68, 0x78, 0x68,
0x60, 0x60, 0x60, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x66,
0xc2, 0xc0, 0xc0, 0xde, 0xc6, 0xc6, 0x66, 0x3a, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0xc6, 0xc6, 0xc6, 0xc6, 0xfe, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x18, 0x18, 0x18, 0x18, 0x18,
0x18, 0x18, 0x18, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e, 0x0c,
0x0c, 0x0c, 0x0c, 0x0c, 0xcc, 0xcc, 0xcc, 0x78, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0xe6, 0x66, 0x66, 0x6c, 0x78, 0x78, 0x6c, 0x66, 0x66, 0xe6,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf0, 0x60, 0x60, 0x60, 0x60, 0x60,
0x60, 0x62, 0x66, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc3, 0xe7,
0xff, 0xff, 0xdb, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0xc6, 0xe6, 0xf6, 0xfe, 0xde, 0xce, 0xc6, 0xc6, 0xc6, 0xc6,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6,
0xc6, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc, 0x66,
0x66, 0x66, 0x7c, 0x60, 0x60, 0x60, 0x60, 0xf0, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xd6, 0xde, 0x7c,
0x0c, 0x0e, 0x00, 0x00, 0x00, 0x00, 0xfc, 0x66, 0x66, 0x66, 0x7c, 0x6c,
0x66, 0x66, 0x66, 0xe6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0xc6,
0xc6, 0x60, 0x38, 0x0c, 0x06, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0xff, 0xdb, 0x99, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3c,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6,
0xc6, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc3, 0xc3,
0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0x66, 0x3c, 0x18, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0xc3, 0xc3, 0xc3, 0xc3, 0xc3, 0xdb, 0xdb, 0xff, 0x66, 0x66,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc3, 0xc3, 0x66, 0x3c, 0x18, 0x18,
0x3c, 0x66, 0xc3, 0xc3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc3, 0xc3,
0xc3, 0x66, 0x3c, 0x18, 0x18, 0x18, 0x18, 0x3c, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0xff, 0xc3, 0x86, 0x0c, 0x18, 0x30, 0x60, 0xc1, 0xc3, 0xff,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x30, 0x30, 0x30, 0x30, 0x30,
0x30, 0x30, 0x30, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
0xc0, 0xe0, 0x70, 0x38, 0x1c, 0x0e, 0x06, 0x02, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x3c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x3c,
0x00, 0x00, 0x00, 0x00, 0x10, 0x38, 0x6c, 0xc6, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
0x30, 0x30, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0x0c, 0x7c,
0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0x60,
0x60, 0x78, 0x6c, 0x66, 0x66, 0x66, 0x66, 0x7c, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0xc6, 0xc0, 0xc0, 0xc0, 0xc6, 0x7c,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x0c, 0x0c, 0x3c, 0x6c, 0xcc,
0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x7c, 0xc6, 0xfe, 0xc0, 0xc0, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x38, 0x6c, 0x64, 0x60, 0xf0, 0x60, 0x60, 0x60, 0x60, 0xf0,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x76, 0xcc, 0xcc,
0xcc, 0xcc, 0xcc, 0x7c, 0x0c, 0xcc, 0x78, 0x00, 0x00, 0x00, 0xe0, 0x60,
0x60, 0x6c, 0x76, 0x66, 0x66, 0x66, 0x66, 0xe6, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x18, 0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3c,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06, 0x00, 0x0e, 0x06, 0x06,
0x06, 0x06, 0x06, 0x06, 0x66, 0x66, 0x3c, 0x00, 0x00, 0x00, 0xe0, 0x60,
0x60, 0x66, 0x6c, 0x78, 0x78, 0x6c, 0x66, 0xe6, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3c,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe6, 0xff, 0xdb,
0xdb, 0xdb, 0xdb, 0xdb, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0xdc, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x7c,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xdc, 0x66, 0x66,
0x66, 0x66, 0x66, 0x7c, 0x60, 0x60, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x76, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x7c, 0x0c, 0x0c, 0x1e, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0xdc, 0x76, 0x66, 0x60, 0x60, 0x60, 0xf0,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c, 0xc6, 0x60,
0x38, 0x0c, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x30,
0x30, 0xfc, 0x30, 0x30, 0x30, 0x30, 0x36, 0x1c, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x76,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc3, 0xc3, 0xc3,
0xc3, 0x66, 0x3c, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0xc3, 0xc3, 0xc3, 0xdb, 0xdb, 0xff, 0x66, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0xc3, 0x66, 0x3c, 0x18, 0x3c, 0x66, 0xc3,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc6, 0xc6, 0xc6,
0xc6, 0xc6, 0xc6, 0x7e, 0x06, 0x0c, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0xfe, 0xcc, 0x18, 0x30, 0x60, 0xc6, 0xfe, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x0e, 0x18, 0x18, 0x18, 0x70, 0x18, 0x18, 0x18, 0x18, 0x0e,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0x18, 0x00, 0x18,
0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x18,
0x18, 0x18, 0x0e, 0x18, 0x18, 0x18, 0x18, 0x70, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x76, 0xdc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x38, 0x6c, 0xc6,
0xc6, 0xc6, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x66,
0xc2, 0xc0, 0xc0, 0xc0, 0xc2, 0x66, 0x3c, 0x0c, 0x06, 0x7c, 0x00, 0x00,
0x00, 0x00, 0xcc, 0x00, 0x00, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x76,
0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x18, 0x30, 0x00, 0x7c, 0xc6, 0xfe,
0xc0, 0xc0, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x38, 0x6c,
0x00, 0x78, 0x0c, 0x7c, 0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0xcc, 0x00, 0x00, 0x78, 0x0c, 0x7c, 0xcc, 0xcc, 0xcc, 0x76,
0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x30, 0x18, 0x00, 0x78, 0x0c, 0x7c,
0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x6c, 0x38,
0x00, 0x78, 0x0c, 0x7c, 0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x3c, 0x66, 0x60, 0x60, 0x66, 0x3c, 0x0c, 0x06,
0x3c, 0x00, 0x00, 0x00, 0x00, 0x10, 0x38, 0x6c, 0x00, 0x7c, 0xc6, 0xfe,
0xc0, 0xc0, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc6, 0x00,
0x00, 0x7c, 0xc6, 0xfe, 0xc0, 0xc0, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
0x00, 0x60, 0x30, 0x18, 0x00, 0x7c, 0xc6, 0xfe, 0xc0, 0xc0, 0xc6, 0x7c,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x66, 0x00, 0x00, 0x38, 0x18, 0x18,
0x18, 0x18, 0x18, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x3c, 0x66,
0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3c, 0x00, 0x00, 0x00, 0x00,
0x00, 0x60, 0x30, 0x18, 0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3c,
0x00, 0x00, 0x00, 0x00, 0x00, 0xc6, 0x00, 0x10, 0x38, 0x6c, 0xc6, 0xc6,
0xfe, 0xc6, 0xc6, 0xc6, 0x00, 0x00, 0x00, 0x00, 0x38, 0x6c, 0x38, 0x00,
0x38, 0x6c, 0xc6, 0xc6, 0xfe, 0xc6, 0xc6, 0xc6, 0x00, 0x00, 0x00, 0x00,
0x18, 0x30, 0x60, 0x00, 0xfe, 0x66, 0x60, 0x7c, 0x60, 0x60, 0x66, 0xfe,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6e, 0x3b, 0x1b,
0x7e, 0xd8, 0xdc, 0x77, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3e, 0x6c,
0xcc, 0xcc, 0xfe, 0xcc, 0xcc, 0xcc, 0xcc, 0xce, 0x00, 0x00, 0x00, 0x00,
0x00, 0x10, 0x38, 0x6c, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x7c,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc6, 0x00, 0x00, 0x7c, 0xc6, 0xc6,
0xc6, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x30, 0x18,
0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00,
0x00, 0x30, 0x78, 0xcc, 0x00, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x76,
0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x30, 0x18, 0x00, 0xcc, 0xcc, 0xcc,
0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc6, 0x00,
0x00, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x7e, 0x06, 0x0c, 0x78, 0x00,
0x00, 0xc6, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x7c,
0x00, 0x00, 0x00, 0x00, 0x00, 0xc6, 0x00, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6,
0xc6, 0xc6, 0xc6, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x7e,
0xc3, 0xc0, 0xc0, 0xc0, 0xc3, 0x7e, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00,
0x00, 0x38, 0x6c, 0x64, 0x60, 0xf0, 0x60, 0x60, 0x60, 0x60, 0xe6, 0xfc,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc3, 0x66, 0x3c, 0x18, 0xff, 0x18,
0xff, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfc, 0x66, 0x66,
0x7c, 0x62, 0x66, 0x6f, 0x66, 0x66, 0x66, 0xf3, 0x00, 0x00, 0x00, 0x00,
0x00, 0x0e, 0x1b, 0x18, 0x18, 0x18, 0x7e, 0x18, 0x18, 0x18, 0x18, 0x18,
0xd8, 0x70, 0x00, 0x00, 0x00, 0x18, 0x30, 0x60, 0x00, 0x78, 0x0c, 0x7c,
0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x18, 0x30,
0x00, 0x38, 0x18, 0x18, 0x18, 0x18, 0x18, 0x3c, 0x00, 0x00, 0x00, 0x00,
0x00, 0x18, 0x30, 0x60, 0x00, 0x7c, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x7c,
0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x30, 0x60, 0x00, 0xcc, 0xcc, 0xcc,
0xcc, 0xcc, 0xcc, 0x76, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x76, 0xdc,
0x00, 0xdc, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x00, 0x00, 0x00, 0x00,
0x76, 0xdc, 0x00, 0xc6, 0xe6, 0xf6, 0xfe, 0xde, 0xce, 0xc6, 0xc6, 0xc6,
0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x6c, 0x6c, 0x3e, 0x00, 0x7e, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x6c, 0x6c,
0x38, 0x00, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x30, 0x30, 0x00, 0x30, 0x30, 0x60, 0xc0, 0xc6, 0xc6, 0x7c,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0xc0,
0xc0, 0xc0, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0xfe, 0x06, 0x06, 0x06, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0xc0, 0xc0, 0xc2, 0xc6, 0xcc, 0x18, 0x30, 0x60, 0xce, 0x9b, 0x06,
0x0c, 0x1f, 0x00, 0x00, 0x00, 0xc0, 0xc0, 0xc2, 0xc6, 0xcc, 0x18, 0x30,
0x66, 0xce, 0x96, 0x3e, 0x06, 0x06, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18,
0x00, 0x18, 0x18, 0x18, 0x3c, 0x3c, 0x3c, 0x18, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x6c, 0xd8, 0x6c, 0x36, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd8, 0x6c, 0x36,
0x6c, 0xd8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0x44, 0x11, 0x44,
0x11, 0x44, 0x11, 0x44, 0x11, 0x44, 0x11, 0x44, 0x11, 0x44, 0x11, 0x44,
0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa,
0x55, 0xaa, 0x55, 0xaa, 0xdd, 0x77, 0xdd, 0x77, 0xdd, 0x77, 0xdd, 0x77,
0xdd, 0x77, 0xdd, 0x77, 0xdd, 0x77, 0xdd, 0x77, 0x18, 0x18, 0x18, 0x18,
0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0xf8, 0x18, 0x18, 0x18, 0x18,
0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0xf8, 0x18, 0xf8,
0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x36, 0x36, 0x36, 0x36,
0x36, 0x36, 0x36, 0xf6, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0x36, 0x36, 0x36, 0x36,
0x36, 0x36, 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x18, 0xf8,
0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x36, 0x36, 0x36, 0x36,
0x36, 0xf6, 0x06, 0xf6, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
0x36, 0x36, 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0x06, 0xf6,
0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
0x36, 0xf6, 0x06, 0xfe, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0xfe, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0x18, 0x18, 0xf8, 0x18, 0xf8,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0xf8, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1f, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0xff,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0xff, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1f, 0x18, 0x18, 0x18, 0x18,
0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0x18,
0x18, 0x18, 0x18, 0xff, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
0x18, 0x18, 0x18, 0x18, 0x18, 0x1f, 0x18, 0x1f, 0x18, 0x18, 0x18, 0x18,
0x18, 0x18, 0x18, 0x18, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x37,
0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
0x36, 0x37, 0x30, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x30, 0x37, 0x36, 0x36, 0x36, 0x36,
0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0xf7, 0x00, 0xff,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0xff, 0x00, 0xf7, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
0x36, 0x36, 0x36, 0x36, 0x36, 0x37, 0x30, 0x37, 0x36, 0x36, 0x36, 0x36,
0x36, 0x36, 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x36, 0x36, 0x36, 0x36,
0x36, 0xf7, 0x00, 0xf7, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
0x18, 0x18, 0x18, 0x18, 0x18, 0xff, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0xff,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0xff, 0x00, 0xff, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x36, 0x36, 0x36, 0x36,
0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x3f,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x18, 0x18,
0x18, 0x1f, 0x18, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0x18, 0x1f, 0x18, 0x18, 0x18, 0x18,
0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f,
0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
0x36, 0x36, 0x36, 0xff, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36, 0x36,
0x18, 0x18, 0x18, 0x18, 0x18, 0xff, 0x18, 0xff, 0x18, 0x18, 0x18, 0x18,
0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0xf8,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x1f, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf0, 0xf0, 0xf0, 0xf0,
0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0, 0xf0,
0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f,
0x0f, 0x0f, 0x0f, 0x0f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x76, 0xdc, 0xd8, 0xd8, 0xd8, 0xdc, 0x76, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x78, 0xcc, 0xcc, 0xcc, 0xd8, 0xcc, 0xc6, 0xc6, 0xc6, 0xcc,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0xc6, 0xc6, 0xc0, 0xc0, 0xc0,
0xc0, 0xc0, 0xc0, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0xfe, 0x6c, 0x6c, 0x6c, 0x6c, 0x6c, 0x6c, 0x6c, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0xfe, 0xc6, 0x60, 0x30, 0x18, 0x30, 0x60, 0xc6, 0xfe,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0xd8, 0xd8,
0xd8, 0xd8, 0xd8, 0x70, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x66, 0x66, 0x66, 0x66, 0x66, 0x7c, 0x60, 0x60, 0xc0, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x76, 0xdc, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7e, 0x18, 0x3c, 0x66, 0x66,
0x66, 0x3c, 0x18, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38,
0x6c, 0xc6, 0xc6, 0xfe, 0xc6, 0xc6, 0x6c, 0x38, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x38, 0x6c, 0xc6, 0xc6, 0xc6, 0x6c, 0x6c, 0x6c, 0x6c, 0xee,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e, 0x30, 0x18, 0x0c, 0x3e, 0x66,
0x66, 0x66, 0x66, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x7e, 0xdb, 0xdb, 0xdb, 0x7e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x03, 0x06, 0x7e, 0xdb, 0xdb, 0xf3, 0x7e, 0x60, 0xc0,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x30, 0x60, 0x60, 0x7c, 0x60,
0x60, 0x60, 0x30, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7c,
0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0xc6, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0xfe, 0x00, 0x00, 0xfe, 0x00, 0x00, 0xfe, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x7e, 0x18,
0x18, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30,
0x18, 0x0c, 0x06, 0x0c, 0x18, 0x30, 0x00, 0x7e, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x0c, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0c, 0x00, 0x7e,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0e, 0x1b, 0x1b, 0x1b, 0x18, 0x18,
0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
0x18, 0x18, 0x18, 0x18, 0xd8, 0xd8, 0xd8, 0x70, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x7e, 0x00, 0x18, 0x18, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x76, 0xdc, 0x00,
0x76, 0xdc, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x6c, 0x6c,
0x38, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0f, 0x0c, 0x0c,
0x0c, 0x0c, 0x0c, 0xec, 0x6c, 0x6c, 0x3c, 0x1c, 0x00, 0x00, 0x00, 0x00,
0x00, 0xd8, 0x6c, 0x6c, 0x6c, 0x6c, 0x6c, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0xd8, 0x30, 0x60, 0xc8, 0xf8, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x7c, 0x7c, 0x7c, 0x7c, 0x7c, 0x7c, 0x7c, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00,
};

