/*
 * This file is part of the zlog Library.
 *
 * Copyright (C) 2011 by Hardy Simpson <HardySimpson1984@gmail.com>
 *
 * Licensed under the LGPL v2.1, see the file COPYING in base directory.
 */

#include "fmacros.h"

#include <string.h>
#include <ctype.h>
#include <syslog.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

#include "zc_defs.h"
#include "format.h"
#include "thread.h"
#include "record.h"
  #include "rule.h"

#include "format.h"
#include "buf.h"
#include "thread.h"
#include "level_list.h"
#include "rotater.h"
#include "spec.h"
#include "conf.h"

#include "zc_defs.h"


void zlog_rule_profile(zlog_rule_t * a_rule, int flag)
{
	int i;
	zlog_spec_t *a_spec;

	zc_assert(a_rule,);
	zc_profile(flag, "---rule:[%p][%s%c%d]-[%d,%d][%s,%p,%d:%ld*%d~%s][%d][%d][%s:%s:%p];[%p]---",
		a_rule,

		a_rule->category,
		a_rule->compare_char,
		a_rule->level,

		a_rule->file_perms,
		a_rule->file_open_flags,

		a_rule->file_path,
		a_rule->dynamic_specs,
		a_rule->static_fd,

		a_rule->archive_max_size,
		a_rule->archive_max_count,
		a_rule->archive_path,

		a_rule->pipe_fd,

		a_rule->syslog_facility,

		a_rule->record_name,
		a_rule->record_path,
		a_rule->record_func,
		a_rule->format);

	if (a_rule->dynamic_specs) {
		zc_arraylist_foreach(a_rule->dynamic_specs, i, a_spec) {
			zlog_spec_profile(a_spec, flag);
		}
	}
	return;
}
/*******************************************************************************/
static int zlog_rule_flush_file(zlog_rule_t * a_rule)
{
	int rc;
	rc = write(a_rule->file_fd, a_rule->buffer, zc_sdslen(a_rule_buffer));
	a_rule->flush_count = 0;
	zc_sdsclear(a_rule->buffer);
	if (rc < 0) { zc_error("write fail, errno[%d]", errno); return -1; }
	return 0;
}

/*******************************************************************************/

static int zlog_rule_output_static_file_single(zlog_rule_t * a_rule, zlog_event_t *a_event, zlog_mdc_t *a_mdc)
{
	int rc;
	size_t len;
	zlog_deepness_t *deep = a_rule->file_deep;
	int need_flush;
	int need_fsync;

	rc = zlog_format_gen_msg(a_rule->format, a_event, a_mdc, a_rule->buffer);
	if (!rc) { zc_error("zlog_format_gen_msg fail"); return -1; }
	len = zc_sdslen(a_rule->buffer);

	need_flush = (deep->flush_count && ++a_rule->flush_count >= deep->flush_count) || (len > deep->flush_len);
	need_fsync = deep->fsync_count && ++a_rule->fsync_count >= deep->fsync_count;

	if ( need_fsync || need_flush) {
		rc = zlog_rule_flush_file(a_rule);
		if (!rc) { zc_error("zlog_rule_flush_file fail"); return -1; }
	}

	if (need_fsync) {
		a_rule->fsync_count = 0;
		rc = fsync(a_rule->file_fd);
		if (rc) { zc_error("fsync fail, errno[%d]", errno); return -1 }
	}

	return 0;
}

#define zlog_rule_gen_archive_path(a_rule, a_event, a_mdc)  \
	if (a_rule->archive_specs) do { \
		int i;  \
		zlog_spec_t *a_spec; \
		zc_sdsclear(a_rule->archive_path);  \
		zc_arraylist_foreach(a_rule->archive_specs, i, a_spec) {  \
			rc = zlog_spec_gen(a_spec, a_event, a_mdc, a_rule->archive_path);  \
			if (rc) { zc_error("zlog_spec_gen fail"); return -1; }  \
		}  \
	} while(0);

static int zlog_rule_reopen_file(zlog_rule_t * a_rule)
{
	int rc;

	rc = close(a_rule->file_fd);
	if (rc) { zc_warn("close [%d] fail, errno[%d]", a_rule->file_fd, errno); }
	a_rule->file_fd = open(a_rule->file_path, O_WRONLY | O_APPEND | O_CREAT, a_rule->file_deep->perms);
	if (fd < 0) { zc_error("open file[%s] fail, errno[%d]", a_rule->file_path, errno); return -1; }
	return 0;
}


static int zlog_rule_output_static_file_rotate(zlog_rule_t * a_rule, zlog_event_t *a_event, zlog_mdc_t *a_mdc) 
{
	int rc;
	struct zlog_stat info;
	size_t len;
	int need_reopen;
	int need_flush;
	int need_fsync;

	rc = zlog_format_gen_msg(a_rule->format, a_event, a_mdc, a_rule->buffer);
	if (!rc) { zc_error("zlog_format_gen_msg fail"); return -1; }
	len = zc_sdslen(a_rule->buffer);

	rc = zlog_fstat(a_rule->file_fd, &info);
	if (rc) { zc_warn("fstat [%d] fail, errno[%d]", a_rule->file_fd, errno); return -1; }
	/* fd_big_enough just means file of file_fd now is big enough,
	 * but it is possible that this fd point to xx.log.1, which is moved by other process,
	 * so the size will be judged again in zlog_rotater_rotate() from path(but not fd).
	 * no matter which case, this fd should be closed and reopen.
	 */
	need_reopen = (info.st_size + len > a_rule->archive_max_size);
	need_flush = (deep->flush_count && ++a_rule->flush_count >= deep->flush_count) || (len > deep->flush_len);
	need_fsync = deep->fsync_count && ++a_rule->fsync_count >= deep->fsync_count;

	
	if (need_fsync || need_flush || need_reopen) {
		rc = zlog_rule_flush_file(a_rule);
		if (!rc) { zc_error("zlog_rule_flush_file fail"); return -1; }
	}

	if (need_fsync) {
		a_rule->fsync_count = 0;
		rc = zlog_fsync(a_rule->file_fd);
		if (rc) { zc_error("fsync fail, errno[%d]", errno); return -1 }
	}

	if (need_reopen) {
		zlog_rule_gen_archive_path(a_rule, a_event, a_mdc);

		rc = zlog_rotater_rotate(zlog_env_conf->rotater, 
			a_rule->file_path, a_rule->archive_path,
			a_rule->archive_max_size, a_rule->archive_max_count)
		if (rc) { zc_error("zlog_rotater_rotate fail"); return -1; }

		rc = zlog_rule_reopen_file(a_rule);
		if (rc) { zc_error("zlog_reopen fail"); return -1 }
	}

	return 0;
}

#define zlog_rule_gen_file_path_dynamic(a_rule, a_event, a_mdc) do {    \
		int i;    \
		zlog_spec_t *a_spec;    \
		zc_sdsclear(a_rule->file_path_dynamic);    \
		zc_arraylist_foreach(a_rule->file_path_specs, i, a_spec) {    \
			rc = zlog_spec_gen(a_spec, a_event, a_mdc, a_rule->file_path_dynamic);
			if (rc) { zc_error("zlog_spec_gen fail"); return -1; }    \
		}    \
	} while(0)


static int zlog_rule_output_dynamic_file_single(zlog_rule_t * a_rule, zlog_event_t *a_event, zlog_mdc_t *a_mdc)
{
	int rc;
	size_t len;
	int need_flush;
	int need_fsync;

	zlog_rule_gen_file_path_dynamic(a_rule, a_event, a_mdc);

	if (STRCMP(file_path_dynamic, !=, file_path)) {
		rc = zlog_rule_flush_file(a_rule);
		if (!rc) { zc_error("zlog_rule_flush_file fail"); return -1; }
		
		a_rule->file_path = zc_sdscpylen(a_rule->file_path,
			a_rule->file_path_dynamic, zc_sdslen(a_rule->file_path_dynamic));
		if (!a_rule->file_path) { zc_error("zc_sdscpylen fail, errno[%d]", errno); return -1; }

		rc = zlog_rule_reopen_file(a_rule);
		if (rc) { zc_error("zlog_reopen fail"); return -1 }
	}

	rc = zlog_format_gen_msg(a_rule->format, a_event, a_mdc, a_rule->buffer);
	if (!rc) { zc_error("zlog_format_gen_msg fail"); return -1; }
	len = zc_sdslen(a_rule->buffer);

	need_flush = (deep->flush_count && ++a_rule->flush_count >= deep->flush_count) || (len > deep->flush_len);
	need_fsync = deep->fsync_count && ++a_rule->fsync_count >= deep->fsync_count;

	if ( need_fsync || need_flush) {
		rc = zlog_rule_flush_file(a_rule);
		if (!rc) { zc_error("zlog_rule_flush_file fail"); return -1; }
	}

	if (need_fsync) {
		a_rule->fsync_count = 0;
		rc = fsync(a_rule->file_fd);
		if (rc) { zc_error("fsync fail, errno[%d]", errno); return -1 }
	}


	return 0;
}

static int zlog_rule_output_dynamic_file_rotate(zlog_rule_t * a_rule, zlog_event_t *a_event, zlog_mdc_t *a_mdc)
{
	int rc;
	size_t len;
	struct zlog_stat info;
	int need_reopen;
	int need_flush;
	int need_fsync;

	zlog_rule_gen_file_path_dynamic(a_rule, a_event, a_mdc);

	if (STRCMP(file_path_dynamic, !=, file_path)) {
		rc = zlog_rule_flush_file(a_rule);
		if (!rc) { zc_error("zlog_rule_flush_file fail"); return -1; }
		
		a_rule->file_path = zc_sdscpylen(a_rule->file_path,
			a_rule->file_path_dynamic, zc_sdslen(a_rule->file_path_dynamic));
		if (!a_rule->file_path) { zc_error("zc_sdscpylen fail, errno[%d]", errno); return -1; }

		rc = zlog_rule_reopen_file(a_rule);
		if (rc) { zc_error("zlog_reopen fail"); return -1 }
	}

	rc = zlog_format_gen_msg(a_rule->format, a_event, a_mdc, a_rule->buffer);
	if (!rc) { zc_error("zlog_format_gen_msg fail"); return -1; }
	len = zc_sdslen(a_rule->buffer);

	rc = zlog_fstat(a_rule->file_fd, &info);
	if (rc) { zc_warn("fstat [%d] fail, errno[%d]", a_rule->file_fd, errno); return -1; }
	/* fd_big_enough just means file of file_fd now is big enough,
	 * but it is possible that this fd point to xx.log.1, which is moved by other process,
	 * so the size will be judged again in zlog_rotater_rotate() from path(but not fd).
	 * no matter which case, this fd should be closed and reopen.
	 */
	need_reopen = (info.st_size + len > a_rule->archive_max_size);
	need_flush = (deep->flush_count && ++a_rule->flush_count >= deep->flush_count) || (len > deep->flush_len);
	need_fsync = deep->fsync_count && ++a_rule->fsync_count >= deep->fsync_count;

	
	if (need_fsync || need_flush || need_reopen) {
		rc = zlog_rule_flush_file(a_rule);
		if (!rc) { zc_error("zlog_rule_flush_file fail"); return -1; }
	}

	if (need_fsync) {
		a_rule->fsync_count = 0;
		rc = zlog_fsync(a_rule->file_fd);
		if (rc) { zc_error("fsync fail, errno[%d]", errno); return -1 }
	}

	if (need_reopen) {
		zlog_rule_gen_archive_path(a_rule, a_event, a_mdc);

		rc = zlog_rotater_rotate(zlog_env_conf->rotater, 
			a_rule->file_path, a_rule->archive_path,
			a_rule->archive_max_size, a_rule->archive_max_count)
		if (rc) { zc_error("zlog_rotater_rotate fail"); return -1; }

		rc = zlog_rule_reopen_file(a_rule);
		if (rc) { zc_error("zlog_reopen fail"); return -1 }
	}

	return 0;
}

static int zlog_rule_output_pipe(zlog_rule_t * a_rule, zlog_event_t *a_event, zlog_mdc_t *a_mdc)
{
	int rc;
	size_t len;
	int need_flush;

	rc = zlog_format_gen_msg(a_rule->format, a_event, a_mdc, a_rule->buffer);
	if (!rc) { zc_error("zlog_format_gen_msg fail"); return -1; }
	len = zc_sdslen(a_rule->buffer);

	need_flush = (deep->flush_count && ++a_rule->flush_count >= deep->flush_count) || (len > deep->flush_len);
	if (need_flush) {
		rc = zlog_rule_flush_file(a_rule);
		if (!rc) { zc_error("zlog_rule_flush_file fail"); return -1; }
	}

	return 0;
}
/*******************************************************************************/
static int zlog_rule_flush_donothing()
{
	return 0;
}
/*******************************************************************************/

static int zlog_rule_output_syslog(zlog_rule_t * a_rule, zlog_event_t *a_event, zlog_mdc_t *a_mdc)
{
	int rc;
	zlog_level_t *a_level;

	zc_sdsclear(a_rule->buffer);
	rc = zlog_format_gen_msg(a_rule->format, a_event, a_mdc, a_rule->buffer);
	if (!rc) { zc_error("zlog_format_gen_msg fail"); return -1; }

	a_level = zlog_level_list_get(a_rule->levels, a_event->level);
	syslog(a_rule->syslog_facility | a_level->syslog_level, "%s",  a_rule->buffer);
	return 0;
}

static int zlog_rule_output_static_record(zlog_rule_t * a_rule, zlog_event_t *a_event, zlog_mdc_t *a_mdc)
{
	int rc;

	if (!a_rule->record_func) {
		zc_error("user defined record funcion for [%s] not set, no output",
				a_rule->record_func_name);
		return -1;
	}

	zc_sdsclear(a_rule->buffer);
	rc = zlog_format_gen_msg(a_rule->format, a_event, a_mdc, a_rule->buffer);
	if (!rc) { zc_error("zlog_format_gen_msg fail"); return -1; }

	rc = a_rule->record_func(a_rule->buffer, zc_sdslen(a_rule->buffer),
			a_rule->file_path, zc_sdslen(a_rule->file_path));
	if (rc) { zc_error("a_rule->record fail"); return -1; }
	return 0;
}

static int zlog_rule_output_dynamic_record(zlog_rule_t * a_rule, zlog_thread_t * a_thread)
{
	int rc;

	if (!a_rule->record_func) {
		zc_error("user defined record funcion for [%s] not set, no output",
				a_rule->record_func_name);
		return -1;
	}

	zlog_rule_gen_file_path_dynamic(a_rule, a_event, a_mdc);

	zc_sdsclear(a_rule->buffer);
	rc = zlog_format_gen_msg(a_rule->format, a_event, a_mdc, a_rule->buffer);
	if (!rc) { zc_error("zlog_format_gen_msg fail"); return -1; }

	rc = a_rule->record_func(a_rule->buffer, zc_sdslen(a_rule->buffer),
			a_rule->file_path_dynamic, zc_sdslen(a_rule->file_path_dynamic));
	if (rc) { zc_error("a_rule->record fail"); return -1; }

	return 0;
}

static int zlog_rule_output_stdout(zlog_rule_t * a_rule, zlog_thread_t * a_thread)
{
	int rc;

	zc_sdsclear(a_rule->buffer);
	rc = zlog_format_gen_msg(a_rule->format, a_event, a_mdc, a_rule->buffer);
	if (!rc) { zc_error("zlog_format_gen_msg fail"); return -1; }

	rc = write(STDOUT_FILENO, a_rule->buffer, zc_sdslen(a_rule->buffer));
	if (rc < 0) { zc_error("write fail, errno[%d]", errno); return -1; }

	return 0;
}

static int zlog_rule_output_stderr(zlog_rule_t * a_rule, zlog_thread_t * a_thread)
{
	int rc;

	zc_sdsclear(a_rule->buffer);
	rc = zlog_format_gen_msg(a_rule->format, a_event, a_mdc, a_rule->buffer);
	if (!rc) { zc_error("zlog_format_gen_msg fail"); return -1; }

	rc = write(STDERR_FILENO, a_rule->buffer, zc_sdslen(a_rule->buffer));
	if (rc < 0) { zc_error("write fail, errno[%d]", errno); return -1; }

	return 0;
}

/*******************************************************************************/
static int zlog_rule_parse_path(char *path_start, /* start with a " */
		char *path_str, size_t path_size, zc_arraylist_t **path_specs,
		int *time_cache_count)
{
	char *p, *q;
	size_t len;
	zlog_spec_t *a_spec;
	zc_arraylist_t *specs;

	p = path_start + 1;

	q = strrchr(p, '"');
	if (!q) {
		zc_error("matching \" not found in conf line[%s]", path_start);
		return -1;
	}
	len = q - p;
	if (len > path_size - 1) {
		zc_error("file_path too long %ld > %ld", len, path_size - 1);
		return -1;
	}
	memcpy(path_str, p, len);

	/* replace any environment variables like %E(HOME) */
	if (zc_str_replace_env(path_str, path_size)) {
		zc_error("zc_str_replace_env fail");
		return -1;
	}

	if (strchr(path_str, '%') == NULL) {
		/* static, no need create specs */
		return 0;
	}

	specs = zc_arraylist_new((zc_arraylist_del_fn)zlog_spec_del);
	if (!path_specs) {
		zc_error("zc_arraylist_new fail");
		return -1;
	}

	for (p = path_str; *p != '\0'; p = q) {
		a_spec = zlog_spec_new(p, &q, time_cache_count);
		if (!a_spec) {
			zc_error("zlog_spec_new fail");
			goto err;
		}

		if (zc_arraylist_add(specs, a_spec)) {
			zc_error("zc_arraylist_add fail");
			goto err;
		}
	}

	*path_specs = specs;
	return 0;
err:
	if (specs) zc_arraylist_del(specs);
	if (a_spec) zlog_spec_del(a_spec);
	return -1;
}

zlog_rule_t *zlog_rule_new(char *line,
		zc_arraylist_t *levels,
		zlog_format_t * default_format,
		zc_arraylist_t * formats,
		unsigned int file_perms,
		size_t fsync_period,
		int * time_cache_count)
{
	int rc = 0;
	int nscan = 0;
	int nread = 0;
	zlog_rule_t *a_rule;

	char selector[MAXLEN_CFG_LINE + 1];
	char category[MAXLEN_CFG_LINE + 1];
	char level[MAXLEN_CFG_LINE + 1];

	char *action;
	char output[MAXLEN_CFG_LINE + 1];
	char format_name[MAXLEN_CFG_LINE + 1];
	char file_path[MAXLEN_CFG_LINE + 1];
	char archive_max_size[MAXLEN_CFG_LINE + 1];
	char *file_limit;

	char *p;
	char *q;
	size_t len;

	zc_assert(line, NULL);
	zc_assert(default_format, NULL);
	zc_assert(formats, NULL);

	a_rule = calloc(1, sizeof(zlog_rule_t));
	if (!a_rule) {
		zc_error("calloc fail, errno[%d]", errno);
		return NULL;
	}

	a_rule->file_perms = file_perms;
	a_rule->fsync_period = fsync_period;

	/* line         [f.INFO "%H/log/aa.log", 20MB * 12; MyTemplate]
	 * selector     [f.INFO]
	 * *action      ["%H/log/aa.log", 20MB * 12; MyTemplate]
	 */
	memset(&selector, 0x00, sizeof(selector));
	nscan = sscanf(line, "%s %n", selector, &nread);
	if (nscan != 1) {
		zc_error("sscanf [%s] fail, selector", line);
		goto err;
	}
	action = line + nread;

	/*
	 * selector     [f.INFO]
	 * category     [f]
	 * level        [.INFO]
	 */
	memset(category, 0x00, sizeof(category));
	memset(level, 0x00, sizeof(level));
	nscan = sscanf(selector, " %[^.].%s", category, level);
	if (nscan != 2) {
		zc_error("sscanf [%s] fail, category or level is null",
			 selector);
		goto err;
	}


	/* check and set category */
	for (p = category; *p != '\0'; p++) {
		if ((!isalnum(*p)) && (*p != '_') && (*p != '*') && (*p != '!')) {
			zc_error("category name[%s] character is not in [a-Z][0-9][_!*]",
				 category);
			goto err;
		}
	}

	/* as one line can't be longer than MAXLEN_CFG_LINE, same as category */
	strcpy(a_rule->category, category);

	/* check and set level */
	switch (level[0]) {
	case '=':
		/* aa.=debug */
		a_rule->compare_char = '=';
		p = level + 1;
		break;
	case '!':
		/* aa.!debug */
		a_rule->compare_char = '!';
		p = level + 1;
		break;
	case '*':
		/* aa.* */
		a_rule->compare_char = '*';
		p = level;
		break;
	default:
		/* aa.debug */
		a_rule->compare_char = '.';
		p = level;
		break;
	}

	a_rule->level = zlog_level_list_atoi(levels, p);

	/* level_bit is a bitmap represents which level can be output 
	 * 32bytes, [0-255] levels, see level.c
	 * which bit field is 1 means allow output and 0 not
	 */
	switch (a_rule->compare_char) {
	case '=':
		memset(a_rule->level_bitmap, 0x00, sizeof(a_rule->level_bitmap));
		a_rule->level_bitmap[a_rule->level / 8] |= (1 << (7 - a_rule->level % 8));
		break;
	case '!':
		memset(a_rule->level_bitmap, 0xFF, sizeof(a_rule->level_bitmap));
		a_rule->level_bitmap[a_rule->level / 8] &= ~(1 << (7 - a_rule->level % 8));
		break;
	case '*':
		memset(a_rule->level_bitmap, 0xFF, sizeof(a_rule->level_bitmap));
		break;
	case '.':
		memset(a_rule->level_bitmap, 0x00, sizeof(a_rule->level_bitmap));
		a_rule->level_bitmap[a_rule->level / 8] |= ~(0xFF << (8 - a_rule->level % 8));
		memset(a_rule->level_bitmap + a_rule->level / 8 + 1, 0xFF,
				sizeof(a_rule->level_bitmap) -  a_rule->level / 8 - 1);
		break;
	}

	/* action               ["%H/log/aa.log", 20MB * 12 ; MyTemplate]
	 * output               ["%H/log/aa.log", 20MB * 12]
	 * format               [MyTemplate]
	 */
	memset(output, 0x00, sizeof(output));
	memset(format_name, 0x00, sizeof(format_name));
	nscan = sscanf(action, " %[^;];%s", output, format_name);
	if (nscan < 1) {
		zc_error("sscanf [%s] fail", action);
		goto err;
	}

	/* check and get format */
	if (STRCMP(format_name, ==, "")) {
		zc_debug("no format specified, use default");
		a_rule->format = default_format;
	} else {
		int i;
		int find_flag = 0;
		zlog_format_t *a_format;

		zc_arraylist_foreach(formats, i, a_format) {
			if (zlog_format_has_name(a_format, format_name)) {
				a_rule->format = a_format;
				find_flag = 1;
				break;
			}
		}
		if (!find_flag) {
			zc_error("in conf file can't find format[%s], pls check",
			     format_name);
			goto err;
		}
	}

	/* output               [-"%E(HOME)/log/aa.log" , 20MB*12]  [>syslog , LOG_LOCAL0 ]
	 * file_path            [-"%E(HOME)/log/aa.log" ]           [>syslog ]
	 * *file_limit          [20MB * 12 ~ "aa.#i.log" ]          [LOG_LOCAL0]
	 */
	memset(file_path, 0x00, sizeof(file_path));
	nscan = sscanf(output, " %[^,],", file_path);
	if (nscan < 1) {
		zc_error("sscanf [%s] fail", action);
		goto err;
	}

	file_limit = strchr(output, ',');
	if (file_limit) {
		file_limit++; /* skip the , */
		while( isspace(*file_limit) ) {
			file_limit++;
		}
	}

	p = NULL;
	switch (file_path[0]) {
	case '-' :
		/* sync file each time write log */
		if (file_path[1] != '"') {
			zc_error(" - must set before a file output");
			goto err;
		}

		/* no need to fsync, as file is opened by O_SYNC, write immediately */
		a_rule->fsync_period = 0;

		p = file_path + 1;
		a_rule->file_open_flags = O_SYNC;
		/* fall through */
	case '"' :
		if (!p) p = file_path;

		rc = zlog_rule_parse_path(p, a_rule->file_path, sizeof(a_rule->file_path),
				&(a_rule->dynamic_specs), time_cache_count);
		if (rc) {
			zc_error("zlog_rule_parse_path fail");
			goto err;
		}

		if (file_limit) {
			memset(archive_max_size, 0x00, sizeof(archive_max_size));
			nscan = sscanf(file_limit, " %[0-9MmKkBb] * %d ~",
					archive_max_size, &(a_rule->archive_max_count));
			if (nscan) {
				a_rule->archive_max_size = zc_parse_byte_size(archive_max_size);
			}
			p = strchr(file_limit, '"');
			if (p) { /* archive file path exist */
				rc = zlog_rule_parse_path(p,
					a_rule->archive_path, sizeof(a_rule->file_path),
					&(a_rule->archive_specs), time_cache_count);
				if (rc) {
					zc_error("zlog_rule_parse_path fail");
					goto err;
				}

				p = strchr(a_rule->archive_path, '#');
				if ( (p == NULL) && (
						(strchr(p, 'r') == NULL) || (strchr(p, 's') == NULL)
					)
				   ) {
					zc_error("archive_path must contain #r or #s");
					goto err;
				}
			}
		}

		/* try to figure out if the log file path is dynamic or static */
		if (a_rule->dynamic_specs) {
			if (a_rule->archive_max_size <= 0) {
				a_rule->output = zlog_rule_output_dynamic_file_single;
			} else {
				a_rule->output = zlog_rule_output_dynamic_file_rotate;
			}
		} else {
			if (a_rule->archive_max_size <= 0) {
				a_rule->output = zlog_rule_output_static_file_single;
			} else {
				/* as rotate, so need to reopen everytime */
				a_rule->output = zlog_rule_output_static_file_rotate;
			}

			a_rule->static_fd = open(a_rule->file_path,
				O_WRONLY | O_APPEND | O_CREAT | a_rule->file_open_flags,
				a_rule->file_perms);
			if (a_rule->static_fd < 0) {
				zc_error("open file[%s] fail, errno[%d]", a_rule->file_path, errno);
				goto err;
			}
		}
		break;
	case '|' :
		a_rule->pipe_fp = popen(output + 1, "w");
		if (!a_rule->pipe_fp) {
			zc_error("popen fail, errno[%d]", errno);
			goto err;
		}
		a_rule->pipe_fd = fileno(a_rule->pipe_fp);
		if (a_rule->pipe_fd < 0 ) {
			zc_error("fileno fail, errno[%d]", errno);
			goto err;
		}
		a_rule->output = zlog_rule_output_pipe;
		break;
	case '>' :
		if (STRNCMP(file_path + 1, ==, "syslog", 6)) {
			a_rule->syslog_facility = syslog_facility_atoi(file_limit);
			if (a_rule->syslog_facility == -187) {
				zc_error("-187 get");
				goto err;
			}
			a_rule->output = zlog_rule_output_syslog;
			openlog(NULL, LOG_NDELAY | LOG_NOWAIT | LOG_PID, LOG_USER);
		} else if (STRNCMP(file_path + 1, ==, "stdout", 6)) {
			a_rule->output = zlog_rule_output_stdout;
		} else if (STRNCMP(file_path + 1, ==, "stderr", 6)) {
			a_rule->output = zlog_rule_output_stderr;
		} else {
			zc_error
			    ("[%s]the string after is not syslog, stdout or stderr", output);
			goto err;
		}
		break;
	case '$' :
		sscanf(file_path + 1, "%s", a_rule->record_name);
			
		if (file_limit) {  /* record path exists */
			p = strchr(file_limit, '"');
			if (!p) {
				zc_error("record_path not start with \", [%s]", file_limit);
				goto err;
			}
			p++; /* skip 1st " */

			q = strrchr(p, '"');
			if (!q) {
				zc_error("matching \" not found in conf line[%s]", p);
				goto err;
			}
			len = q - p;
			if (len > sizeof(a_rule->record_path) - 1) {
				zc_error("record_path too long %ld > %ld", len, sizeof(a_rule->record_path) - 1);
				goto err;
			}
			memcpy(a_rule->record_path, p, len);
		}

		/* replace any environment variables like %E(HOME) */
		rc = zc_str_replace_env(a_rule->record_path, sizeof(a_rule->record_path));
		if (rc) {
			zc_error("zc_str_replace_env fail");
			goto err;
		}

		/* try to figure out if the log file path is dynamic or static */
		if (strchr(a_rule->record_path, '%') == NULL) {
			a_rule->output = zlog_rule_output_static_record;
		} else {
			zlog_spec_t *a_spec;

			a_rule->output = zlog_rule_output_dynamic_record;

			a_rule->dynamic_specs = zc_arraylist_new((zc_arraylist_del_fn)zlog_spec_del);
			if (!(a_rule->dynamic_specs)) {
				zc_error("zc_arraylist_new fail");
				goto err;
			}
			for (p = a_rule->record_path; *p != '\0'; p = q) {
				a_spec = zlog_spec_new(p, &q, time_cache_count);
				if (!a_spec) {
					zc_error("zlog_spec_new fail");
					goto err;
				}

				rc = zc_arraylist_add(a_rule->dynamic_specs, a_spec);
				if (rc) {
					zlog_spec_del(a_spec);
					zc_error("zc_arraylist_add fail");
					goto err;
				}
			}
		}
		break;
	default :
		zc_error("the 1st char[%c] of file_path[%s] is wrong",
		       file_path[0], file_path);
		goto err;
	}

	//zlog_rule_profile(a_rule, ZC_DEBUG);
	return a_rule;
err:
	zlog_rule_del(a_rule);
	return NULL;
}
/*******************************************************************************/

zlog_rule_t *zlog_rule_new(char *cname, char compare, int level)
{
	zlog_rule_t *a_rule;
	
	a_rule = calloc(1, sizeof(zlog_rule_t));
	if (!a_rule) { zc_error("calloc fail, errno[%d]", errno); return NULL; }

	a_rule->cname = zc_sdsnew(cname);
	if (!a_rule->cname) { zc_error("zc_sdsnew fail, errno[%d]", errno); return NULL; }

	switch (compare) {
	case '=':
		memset(a_rule->level_bitmap, 0x00, sizeof(a_rule->level_bitmap));
		a_rule->level_bitmap[level / 8] |= (1 << (7 - level % 8));
		break;
	case '!':
		memset(a_rule->level_bitmap, 0xFF, sizeof(a_rule->level_bitmap));
		a_rule->level_bitmap[level / 8] &= ~(1 << (7 - level % 8));
		break;
	case '*':
		memset(a_rule->level_bitmap, 0xFF, sizeof(a_rule->level_bitmap));
		break;
	case '.':
		memset(a_rule->level_bitmap, 0x00, sizeof(a_rule->level_bitmap));
		a_rule->level_bitmap[level / 8] |= ~(0xFF << (8 - level % 8));
		memset(a_rule->level_bitmap + level / 8 + 1, 0xFF,
				sizeof(a_rule->level_bitmap) -  level / 8 - 1);
		break;
	}

	return a_rule;
err:
	zlog_rule_del(a_rule);
	return NULL;
}
/*******************************************************************************/

void zlog_rule_del(zlog_rule_t * a_rule)
{
	zc_assert(a_rule,);
	if (a_rule->dynamic_specs) {
		zc_arraylist_del(a_rule->dynamic_specs);
		a_rule->dynamic_specs = NULL;
	}
	if (a_rule->static_fd) {
		if (close(a_rule->static_fd)) {
			zc_error("close fail, maybe cause by write, errno[%d]", errno);
		}
	}
	if (a_rule->pipe_fp) {
		if (pclose(a_rule->pipe_fp) == -1) {
			zc_error("pclose fail, errno[%d]", errno);
		}
	}
	if (a_rule->archive_specs) {
		zc_arraylist_del(a_rule->archive_specs);
		a_rule->archive_specs = NULL;
	}
	free(a_rule);
	zc_debug("zlog_rule_del[%p]", a_rule);
	return;
}

/*******************************************************************************/
static int zlog_syslog_facility_atoi(char *facility)
{
	int i;
	static struct {
		const char *name;
		const int value;
	} zlog_syslog_facility[] = {
		{"auth", LOG_AUTH },
		{"authpriv", LOG_AUTHPRIV },
		{"cron", LOG_CRON },
		{"daemon", LOG_DAEMON },
		{"ftp", LOG_FTP },
		{"kern", LOG_KERN },
		{"lpr", LOG_LPR },
		{"mail", LOG_MAIL },
		{"mark", INTERNAL_MARK },		/* INTERNAL */
		{"news", LOG_NEWS },
		{"security", LOG_AUTH },		/* DEPRECATED */
		{"syslog", LOG_SYSLOG },
		{"user", LOG_USER },
		{"uucp", LOG_UUCP },
		{"local0", LOG_LOCAL0 },
		{"local1", LOG_LOCAL1 },
		{"local2", LOG_LOCAL2 },
		{"local3", LOG_LOCAL3 },
		{"local4", LOG_LOCAL4 },
		{"local5", LOG_LOCAL5 },
		{"local6", LOG_LOCAL6 },
		{"local7", LOG_LOCAL7 },
		{ NULL, -1 }
	}	

	for (i = 0; zlog_syslog_facility[i].name; i++) {
		if (STRICMP(facility, ==, zlog_syslog_facility[i].name)) {
			return zlog_syslog_facility[i].vaule;
		}
	}
	return -1;
}

int zlog_rule_set(zlog_rule_t *a_rule, char *key, void *value)
{
	zc_assert(a_rule, -1);
	zc_assert(key, -1);
	zc_assert(value, -1);

	if (STRCMP(key, ==, "syslog_facility")) {
		a_rule->syslog_facility = zlog_syslog_facility_atoi(value);
		if (a_rule->syslog_facility == -1) {
			zc_error("syslog facility[%s] not exist", value);
			return -1;
		}
	} else if (STRCMP(key, ==, "file_path")) {

	}
}
/*******************************************************************************/
int zlog_rule_match_category_name(zlog_rule_t * a_rule, char *category_name)
{
	size_t len = zc_sdslen(a_rule->category_name);

	zc_assert(a_rule, -1);
	zc_assert(category_name, -1);

	if (STRCMP(a_rule->category_name, ==, "*")) {
		/* '*' match anything, so go on */
		return 1;
	} else if (STRCMP(a_rule->category_name, ==, category_name)) {
		/* accurate compare */
		return 1;
	} else if (a_rule->category_name[len - 1] == '_') {
		/* aa_ match aa_xx & aa, but not match aa1_xx */

		if (strlen(category_name) == len - 1) { len--; }

		if (STRNCMP(a_rule->category_name, ==, category_name, len)) {
			return 1;
		}
	}

	return 0;
}

/*******************************************************************************/
