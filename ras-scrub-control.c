/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2021-2021. All rights reserved.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <limits.h>
#include <ctype.h>
#include <dirent.h>
#include "ras-logger.h"
#include "ras-scrub-control.h"

#define SECOND_OF_MON (30 * 24 * 60 * 60)
#define SECOND_OF_DAY (24 * 60 * 60)
#define SECOND_OF_HOU (60 * 60)
#define SECOND_OF_MIN (60)
#define DEC_CHECK 10
#define LAST_BIT_OF_UL 5

#define SCRUB_DIR_PATH_LEN 384

/* CXL patrol scrub definitions */
#define INIT_OF_CXL_CE_THRESHOLD 20
#define LIMIT_OF_CXL_CE_THRESHOLD 1000

#define INIT_OF_CXL_SCRUB_CYCLE_IN_HOUR 10
#define LIMIT_OF_CXL_SCRUB_CYCLE_IN_HOUR 48

struct scrub_info {
	char scrub_dir_name[128];
	char scrub_dir_path[512];
	char scrub_name_path[512];
	char scrub_enable_bg_path[512];
	char scrub_enable_od_path[512];
	char scrub_cycle_range_path[512];
	char scrub_cycle_path[512];
	char scrub_addr_range_base_path[512];
	char scrub_addr_range_size_path[512];
	unsigned long uce_nums;
	unsigned long ce_nums;
	struct link_queue *ce_queue;
	enum scrub_state state;
};


static struct scrub_info *scrub_infos;
static unsigned int nscrubs;
static unsigned int enabled = 1;
static const char *edac_bus_path = "/sys/bus/edac/devices/";
static const char *scrub_name = "name";
static const char *scrub_enable_bg = "enable_background";
static const char *scrub_enable_od = "enable_on_demand";
static const char *scrub_cycle_range = "cycle_in_hours_range";
static const char *scrub_cycle = "cycle_in_hours";
static const char *scrub_addr_range_base = "addr_range_base";
static const char *scrub_addr_range_size = "addr_range_size";

static const struct scrub_param normal_units[] = {
	{"", 1},
	{}
};

static const struct scrub_param cycle_units[] = {
	{"d", SECOND_OF_DAY},
	{"h", SECOND_OF_HOU},
	{"m", SECOND_OF_MIN},
	{"s", 1},
	{}
};

static struct scrub_control_param cxl_ce_threshold = {
	.name = "CXL_CE_THRESHOLD",
	.units = normal_units,
	.value = INIT_OF_CXL_CE_THRESHOLD,
	.limit = LIMIT_OF_CXL_CE_THRESHOLD
};

static struct scrub_control_param cxl_scrub_cycle_in_hour = {
	.name = "CXL_SCRUB_CYCLE_IN_HOUR",
	.units = normal_units,
	.value = INIT_OF_CXL_SCRUB_CYCLE_IN_HOUR,
	.limit = LIMIT_OF_CXL_SCRUB_CYCLE_IN_HOUR
};

static struct scrub_control_param cycle = {
	.name = "SCRUB_ENABLE_CYCLE",
	.units = cycle_units,
	.value = SECOND_OF_DAY,
	.limit = SECOND_OF_MON
};

static int open_sys_file(int __oflag, const char *format)
{
	char real_path[PATH_MAX] = "";
	char path[PATH_MAX] = "";
	int fd;

	snprintf(path, sizeof(path), format);
	if (strlen(path) > PATH_MAX || realpath(path, real_path) == NULL) {
		log(TERM, LOG_ERR, "[%s]:open file: %s failed\n", __func__, path);
		return -1;
	}
	fd = open(real_path, __oflag);
	if (fd == -1) {
		log(TERM, LOG_ERR, "[%s]:open file: %s failed\n", __func__, real_path);
		return -1;
	}

	return fd;
}

static int get_scrub_status(char *file_path)
{
	char buf[2] = "";
	int fd, num;

	fd = open_sys_file(O_RDONLY, file_path);
	if (fd == -1)
		return SCRUB_UNKNOWN;

	if (read(fd, buf, 1) <= 0 || sscanf(buf, "%d", &num) != 1)
		num = SCRUB_UNKNOWN;

	close(fd);

	return (num < 0 || num > SCRUB_UNKNOWN) ? SCRUB_UNKNOWN : num;
}

static int get_edac_scrubber(const char *mem_dev_name)
{
	int index;

	for (index = 0; index < nscrubs; index++) {
		if (strstr(scrub_infos[index].scrub_dir_name, mem_dev_name))
			return index;
	}

	return -1;
}

static int get_edac_scrubs_count(const char *path)
{
	char scrub_dir_path[SCRUB_DIR_PATH_LEN];
	DIR *edac_dir, *scrub_dir = NULL;
	struct dirent *entry;

	edac_dir = opendir(path);
	if (edac_dir == NULL)
		return -1;

	while ((entry = readdir(edac_dir)) != NULL) {
		if (entry->d_type == DT_LNK) {
			memset(scrub_dir_path, 0, SCRUB_DIR_PATH_LEN);
			sprintf(scrub_dir_path, "%s%s/%s/", path, entry->d_name, "scrub");
			scrub_dir = opendir(scrub_dir_path);
			if (scrub_dir) {
				nscrubs++;
				closedir(scrub_dir);
				scrub_dir = NULL;
			}
		}
	}
	closedir(edac_dir);

	return 0;
}

static int edac_scrubs_info_init(const char *path)
{
	char scrub_dir_path[SCRUB_DIR_PATH_LEN];
	DIR *edac_dir, *scrub_dir = NULL;
	struct dirent *entry;
	int rc, count;

	rc = get_edac_scrubs_count(path);
	if (rc < 0)
		return -1;

	scrub_infos = (struct scrub_info *)calloc(nscrubs, sizeof(*scrub_infos));
	if (!scrub_infos) {
		log(TERM, LOG_ERR,
		    "Failed to allocate memory for scrub infos in %s.\n", __func__);
		return -1;
	}

	edac_dir = opendir(path);
	if (edac_dir == NULL)
		return -1;

	count = 0;
	while ((entry = readdir(edac_dir)) != NULL) {
		if (entry->d_type == DT_LNK) {
			memset(scrub_dir_path, 0, SCRUB_DIR_PATH_LEN);
			sprintf(scrub_dir_path, "%s%s/%s/", path, entry->d_name, "scrub");
			scrub_dir = opendir(scrub_dir_path);
			if (scrub_dir) {
				sprintf(scrub_infos[count].scrub_dir_name, "%s", entry->d_name);
				sprintf(scrub_infos[count].scrub_dir_path, "%s", scrub_dir_path);
				sprintf(scrub_infos[count].scrub_name_path, "%s%s",
					scrub_dir_path, scrub_name);
				sprintf(scrub_infos[count].scrub_enable_bg_path, "%s%s",
					scrub_dir_path, scrub_enable_bg);
				sprintf(scrub_infos[count].scrub_enable_od_path, "%s%s",
					scrub_dir_path, scrub_enable_od);
				sprintf(scrub_infos[count].scrub_cycle_range_path, "%s%s",
					scrub_dir_path, scrub_cycle_range);
				sprintf(scrub_infos[count].scrub_cycle_path, "%s%s",
					scrub_dir_path, scrub_cycle);
				sprintf(scrub_infos[count].scrub_addr_range_base_path, "%s%s",
					scrub_dir_path, scrub_addr_range_base);
				sprintf(scrub_infos[count].scrub_addr_range_size_path, "%s%s",
					scrub_dir_path, scrub_addr_range_size);
				scrub_infos[count].ce_nums = 0;
				scrub_infos[count].uce_nums = 0;
				scrub_infos[count].state = get_scrub_status(scrub_infos[count].scrub_enable_bg_path);
				scrub_infos[count].ce_queue = init_queue();
				if (!scrub_infos[count].ce_queue) {
					log(TERM, LOG_ERR,
					    "Failed to allocate memory for scrub ce queue in %s.\n", __func__);
					return -1;
				}
				closedir(scrub_dir);
				scrub_dir = NULL;
				count++;
			}
		}
	}
	closedir(edac_dir);

	return 0;
}


static int init_scrub_info(void)
{
	int rc;

	rc = edac_scrubs_info_init(edac_bus_path);
	if (rc < 0)
		return -1;

	return 0;
}

static void check_config(struct scrub_control_param *config)
{
	if (config->value > config->limit) {
		log(TERM, LOG_WARNING, "Value: %lu exceed limit: %lu, set to limit\n",
		    config->value, config->limit);
		config->value = config->limit;
	}
}

static int parse_ul_config(struct scrub_control_param *config, char *env, unsigned long *value)
{
	char *unit = NULL;
	int env_size, has_unit = 0;

	if (!env || strlen(env) == 0)
		return -1;

	env_size = strlen(env);
	unit = env + env_size - 1;

	if (isalpha(*unit)) {
		has_unit = 1;
		env_size--;
		if (env_size <= 0)
			return -1;
	}

	for (int i = 0; i < env_size; ++i) {
		if (isdigit(env[i])) {
			if (*value > ULONG_MAX / DEC_CHECK ||
			    (*value == ULONG_MAX / DEC_CHECK && env[i] - '0' > LAST_BIT_OF_UL)) {
				log(TERM, LOG_ERR, "%s is out of range: %lu\n", env, ULONG_MAX);
				return -1;
			}
			*value = DEC_CHECK * (*value) + (env[i] - '0');
		} else
			return -1;
	}

	if (!has_unit)
		return 0;

	for (const struct scrub_param *units = config->units; units->name; units++) {
		/* value character and unit character are both valid */
		if (!strcasecmp(unit, units->name)) {
			if (*value > (ULONG_MAX / units->value)) {
				log(TERM, LOG_ERR,
				    "%s is out of range: %lu\n", env, ULONG_MAX);
				return -1;
			}
			*value = (*value) * units->value;
			return 0;
		}
	}
	log(TERM, LOG_ERR, "Invalid unit %s\n", unit);
	return -1;
}

static void init_config(struct scrub_control_param *config)
{
	char *env = getenv(config->name);
	unsigned long value = 0;

	if (parse_ul_config(config, env, &value) < 0) {
		log(TERM, LOG_ERR, "Invalid %s: %s! Use default value %lu.\n",
		    config->name, env, config->value);
		return;
	}

	config->value = value;
	check_config(config);
}

static int check_config_status(void)
{
	char *env = getenv("SCRUB_CONTROL_ENABLE");

	if (!env || strcasecmp(env, "yes"))
		return -1;

	return 0;
}

void ras_scrub_control_init(void)
{
	if (init_scrub_info() < 0 || check_config_status() < 0) {
		enabled = 0;
		log(TERM, LOG_WARNING, "Scrub control is disabled\n");
		return;
	}

	log(TERM, LOG_INFO, "Scrub control is enabled\n");
	init_config(&cxl_ce_threshold);
	init_config(&cxl_scrub_cycle_in_hour);
	init_config(&cycle);
}

void ras_scrub_control_exit(void)
{
	if (scrub_infos) {
		for (int i = 0; i < nscrubs; ++i)
			free_queue(scrub_infos[i].ce_queue);

		free(scrub_infos);
	}
}

static int do_enable_cxl_scrub(enum scrub_type scrub_type,
			       unsigned int scrub_index)
{
	char *file_path;
	char buf[2] = "";
	char cycle_in_hour[3] = "";
	int fd, rc;
	unsigned int val;
	int state;

	/* check wthether scrubbing is already enabled */
	file_path = scrub_infos[scrub_index].scrub_enable_bg_path;
	state = get_scrub_status(file_path);
	if (state == SCRUB_ENABLE) {
		log(TERM, LOG_INFO, "[%s] scrub is already enabled\n",
		    scrub_infos[scrub_index].scrub_dir_name);
		return MEM_ERR_HANDLE_NOTHING;
	}

	/* set scrub rate */
	file_path = scrub_infos[scrub_index].scrub_cycle_path;
	fd = open_sys_file(O_RDWR, file_path);
	if (fd == -1)
		return MEM_ERR_HANDLE_FAILED;

	sprintf(cycle_in_hour, "%d", (unsigned int)cxl_scrub_cycle_in_hour.value);
	rc = write(fd, cycle_in_hour, strlen(cycle_in_hour));
	if (rc < 0) {
		log(TERM, LOG_ERR, "set scrub rate [%s] failed, errno:%d\n",
		    scrub_infos[scrub_index].scrub_dir_name, errno);
		close(fd);
		return MEM_ERR_HANDLE_FAILED;
	}
	/* read back scrub rate and compare */
	memset(cycle_in_hour, 0, strlen(cycle_in_hour));
	if (read(fd, cycle_in_hour, sizeof(val)) <= 0 ||
	    (sscanf(cycle_in_hour, "%x", &val)) < 1)
		val = -1;
	if (val != cxl_scrub_cycle_in_hour.value) {
		log(TERM, LOG_ERR, "set scrub rate [%s] failed, not matching\n",
		    scrub_infos[scrub_index].scrub_dir_name);
		close(fd);
		return MEM_ERR_HANDLE_FAILED;
	}
	close(fd);

	/* enable scrubbing */
	scrub_infos[scrub_index].state = SCRUB_ENABLE_FAILED;
	file_path = scrub_infos[scrub_index].scrub_enable_bg_path;
	fd = open_sys_file(O_RDWR, file_path);
	if (fd == -1)
		return MEM_ERR_HANDLE_FAILED;

	strcpy(buf, "1");
	rc = write(fd, buf, strlen(buf));
	if (rc < 0) {
		log(TERM, LOG_ERR, "[%s] scrub enable failed, errno:%d\n",
		    scrub_infos[scrub_index].scrub_dir_name, errno);
		close(fd);
		return MEM_ERR_HANDLE_FAILED;
	}
	close(fd);

	/* check wthether scrubbing is enabled successfully */
	scrub_infos[scrub_index].state = get_scrub_status(file_path);

	if (scrub_infos[scrub_index].state == SCRUB_ENABLE)
		return MEM_ERR_HANDLE_SUCCEED;

	return MEM_ERR_HANDLE_FAILED;
}

static int do_ce_handler(enum scrub_type scrub_type, unsigned int scrub_index,
			 struct scrub_control_param *threshold)
{
	struct link_queue *queue = scrub_infos[scrub_index].ce_queue;
	unsigned int tmp;
	/*
	 * Since we just count all error numbers in setted cycle, we store the time
	 * and error numbers from current event to the queue, then everytime we
	 * calculate the period from beginning time to ending time, if the period
	 * exceeds setted cycle, we pop the beginning time and error until the period
	 * from new beginning time to ending time is less than cycle.
	 */
	while (queue->head && queue->tail && queue->tail->time - queue->head->time > cycle.value) {
		tmp = queue->head->value;
		if (pop(queue) == 0)
			scrub_infos[scrub_index].ce_nums -= tmp;
	}
	log(TERM, LOG_INFO,
	    "Current number of Corrected Errors in %s in the cycle is %lu\n",
	    scrub_infos[scrub_index].scrub_dir_name, scrub_infos[scrub_index].ce_nums);

	if (scrub_infos[scrub_index].ce_nums >= threshold->value) {
		log(TERM, LOG_INFO,
		    "Corrected Errors exceeded threshold %lu, try to enable %s scrub\n",
			threshold->value, scrub_infos[scrub_index].scrub_dir_name);
		switch (scrub_type) {
		case CXL_PATROL:
			return do_enable_cxl_scrub(scrub_type, scrub_index);
		default:
			return MEM_ERR_HANDLE_NOTHING;
		}
	}
	return MEM_ERR_HANDLE_NOTHING;
}

static int do_uce_handler(enum scrub_type scrub_type, unsigned int scrub_index)
{
	if (scrub_infos[scrub_index].uce_nums > 0) {
		log(TERM, LOG_INFO, "Uncorrected Errors occurred, try to start %s scrub\n",
		    scrub_infos[scrub_index].scrub_dir_name);
		/* Need to start scrubbing? */
	}
	return MEM_ERR_HANDLE_NOTHING;
}

static int error_handler(enum scrub_type scrub_type, unsigned int scrub_index,
			 struct scrub_control_param *threshold,
			 struct mem_error_info *err_info)
{
	int ret = MEM_ERR_HANDLE_NOTHING;

	switch (err_info->err_severity) {
	case MEM_CE:
		ret = do_ce_handler(scrub_type, scrub_index, threshold);
		break;
	case MEM_UCE:
		ret = do_uce_handler(scrub_type, scrub_index);
		break;
	default:
		break;
	}

	return ret;
}

static void record_error_info(unsigned int scrub_index, struct mem_error_info *err_info)
{
	switch (err_info->err_severity) {
	case MEM_CE:
	{
		struct queue_node *node = node_create(err_info->time, err_info->ce_nums);

		if (!node) {
			log(TERM, LOG_ERR, "Fail to allocate memory for queue node\n");
			return;
		}
		push(scrub_infos[scrub_index].ce_queue, node);
		scrub_infos[scrub_index].ce_nums += err_info->ce_nums;
		break;
	}
	case MEM_UCE:
		scrub_infos[scrub_index].uce_nums++;
		break;
	default:
		break;
	}
}

void ras_scrub_record_mem_error(struct mem_error_info *err_info)
{
	int ret;
	int scrub_index;

	if (enabled == 0)
		return;

	switch (err_info->scrub_type) {
	case CXL_PATROL:
		scrub_index = get_edac_scrubber(err_info->dev_name);
		if (scrub_index < 0)
			return;

		record_error_info(scrub_index, err_info);

		ret = error_handler(CXL_PATROL, scrub_index, &cxl_ce_threshold,
				    err_info);
		if (ret == MEM_ERR_HANDLE_NOTHING)
			log(TERM, LOG_INFO, "Do nothing, error threshod to enable %s scrub is not reached\n",
			    scrub_infos[scrub_index].scrub_dir_name);
		else if (ret == MEM_ERR_HANDLE_SUCCEED) {
			log(TERM, LOG_INFO, "Enable %s scrub succeed\n",
			    scrub_infos[scrub_index].scrub_dir_name);
			clear_queue(scrub_infos[scrub_index].ce_queue);
			scrub_infos[scrub_index].ce_nums = 0;
			scrub_infos[scrub_index].uce_nums = 0;
		} else
			log(TERM, LOG_WARNING, "Enable %s scrub fail\n",
			    scrub_infos[scrub_index].scrub_dir_name);
		break;
	default:
		return;
	}
}
