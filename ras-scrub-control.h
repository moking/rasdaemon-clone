/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2024. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __RAS_SCRUB_CONTROL_H
#define __RAS_SCRUB_CONTROL_H

#include "queue.h"

#define MAX_BUF_LEN 1024

struct scrub_param {
	char *name;
	unsigned long value;
};

struct scrub_control_param {
	char *name;
	const struct scrub_param *units;
	unsigned long value;
	unsigned long limit;
};

enum scrub_state {
	SCRUB_OFF,
	SCRUB_ENABLE,
	SCRUB_ENABLE_FAILED,
	SCRUB_UNKNOWN,
};

enum mem_error_handle_result {
	MEM_ERR_HANDLE_FAILED = -1,
	MEM_ERR_HANDLE_SUCCEED,
	MEM_ERR_HANDLE_NOTHING,
};

enum scrub_type {
	UNKNOWN_SCRUB_TYPE,
	CXL_PATROL,
	ACPI_RAS2_PATROL,
	ACPI_RAS2_ON_DEMAND,
	ACPI_ARS,
	MAX_SCRUB_TYPE,
};

enum mem_error_severity {
	MEM_CE = 1,
	MEM_UCE
};

struct mem_error_info {
	unsigned long ce_nums;
	time_t time;
	enum scrub_type scrub_type;
	char dev_name[MAX_BUF_LEN];
	enum mem_error_severity err_severity;
	bool enable_scrub;
	unsigned long long address;
	unsigned long long size;
};

void ras_scrub_control_init(void);
void ras_scrub_record_mem_error(struct mem_error_info *err_info);
void ras_scrub_control_exit(void);

#endif
