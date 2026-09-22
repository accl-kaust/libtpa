/*
 * SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2023, ByteDance Ltd. and/or its Affiliates
 * Author: Yuanhan Liu <liuyuanhan.131@bytedance.com>
 */
#ifndef _ARCHIVE_MAP_H_
#define _ARCHIVE_MAP_H_

#include "lib/utils.h"

#define NR_MAP_ENTRY		(16 * 1024)

/*
 * all tpa id share one single archive map file
 *
 * It lives beside the per-id archive dirs, under the same log root as
 * tpa_log_root_get(), so that it follows $TPA_LOG_ROOT_PREFIX and needs no
 * root to create.
 */
static inline const char *archive_map_path(const char *name, char *buf, int size)
{
	char prefix[PATH_MAX / 2];

	tpa_snprintf(buf, size, "%s/%s",
		     tpa_state_prefix("TPA_LOG_ROOT_PREFIX", "/var/log/tpa", "log",
				      prefix, sizeof(prefix)),
		     name);

	return buf;
}

static inline const char *archive_map_file(void)
{
	static char path[PATH_MAX];

	if (!path[0])
		archive_map_path(".archive_map", path, sizeof(path));

	return path;
}

static inline const char *curr_trace_map_file(void)
{
	static char path[PATH_MAX];

	if (!path[0])
		archive_map_path(".curr_trace_map", path, sizeof(path));

	return path;
}

#define ARCHIVE_MAP_FILE	archive_map_file()
#define CURR_TRACE_MAP_FILE	curr_trace_map_file()

struct archive_map_entry {
	size_t off;
	uint64_t time; /* in unit of us */
	uint32_t size;
	int sid;
	char reserved[40];
	char name[256 - 64];
	char path[256];
};

struct archive_map {
	uint32_t idx;
	char reserved[8192-4];

	struct archive_map_entry entries[NR_MAP_ENTRY];
};

static inline void archive_map_add(struct archive_map *map, size_t off, uint64_t time,
				   uint32_t size, int sid, const char *name, const char *path)
{
	struct archive_map_entry *entry;
	uint32_t idx;

	if (!map)
		return;

	idx = __sync_fetch_and_add_4(&map->idx, 1) % NR_MAP_ENTRY;
	entry = &map->entries[idx];

	entry->off = off;
	entry->time = time;
	entry->size = size;
	entry->sid = sid;
	tpa_snprintf(entry->name, sizeof(entry->name), "%s", name);
	tpa_snprintf(entry->path, sizeof(entry->path), "%s", path);
}

struct archive_map *map_archive_map_file(const char *path);

#endif
