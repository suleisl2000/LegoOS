/*
 * Copyright (c) 2016 Wuklab, Purdue University. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <asm/io.h>
#include <asm/page.h>

#include <lego/mm.h>
#include <lego/string.h>
#include <lego/kernel.h>
#include <lego/memblock.h>

void __init free_area_init_nodes(unsigned long *max_zone_pfn)
{

}

void __init memory_init(void)
{
	arch_zone_init();
}
