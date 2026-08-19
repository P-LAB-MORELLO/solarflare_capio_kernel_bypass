/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright 2018 Mellanox Technologies, Ltd
 */

#include <string.h>
#include <sys/auxv.h>

#include <rte_common.h>
#include <rte_cpuflags.h>

unsigned long
rte_cpu_getauxval(unsigned long type)
{
	unsigned long val = 0;
	if (elf_aux_info((int)type, &val, sizeof(val)) != 0)
		return 0;
	return val;
}

int
rte_cpu_strcmp_auxval(unsigned long type, const char *str)
{
	char buf[64];
	if (elf_aux_info((int)type, buf, sizeof(buf)) != 0)
		return -1;
	return strcmp(buf, str);
}
