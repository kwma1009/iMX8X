/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2016 Google, Inc
 */

#ifndef __FREESCALE_BOARD_SPL_H
#define __FREESCALE_BOARD_SPL_H

void fsl_spi_spl_load_image(u32 offs, unsigned int size, void *vdst);
void fsl_spi_boot(void) __noreturn;

#endif
