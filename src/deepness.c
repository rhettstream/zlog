/*
 * This file is part of the zlog Library.
 *
 * Copyright (C) 2011 by Hardy Simpson <HardySimpson1984@gmail.com>
 *
 * Licensed under the LGPL v2.1, see the file COPYING in base directory.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "zc_defs.h"

#include "deepness.h"

void zlog_deepness_profile(zlog_deepness_t * a_deepness, int flag)
{

	zc_assert(a_deepness,);
	zc_profile(flag, "---deepness[%p][ %s = %u, %ld, %ld, %ld, %ld]---",
		a_deepness,
		a_deepness->sign,
		(long) a_deepness->perm,
		(long) a_deepness->buffer_len,
		(long) a_deepness->flush_len,
		(long) a_deepness->flush_count,
		(long) a_deepness->fsync_count);

	return;
}

/*******************************************************************************/
void zlog_deepness_del(zlog_deepness_t * a_deepness)
{
	zc_assert(a_deepness,);
	if (a_deepness->sign) zc_sdsfree(a_deepness->sign);
	free(a_deepness);
	zc_debug("zlog_deepness_del[%p]", a_deepness);
	return;
}

zlog_deepness_t *zlog_deepness_new(char *line)
{
	zlog_deepness_t *a_deepness = NULL;
	int argc;
	zc_sds *argv = NULL;
	int e;

	zc_assert(line, NULL);
	a_deepness = calloc(1, sizeof(zlog_deepness_t));
	if (!a_deepness) {
		zc_error("calloc fail, errno[%d]", errno);
		return NULL;
	}

	/* line         ++ = 600, 10KB, 8KB, 20, 1000
	   sign		++
	   perm         600
	   buffer_len  10KB
	   flush_len   8KB
	   flush_count  20
	   fsync_count  1000
	 */
	argv = zc_sdssplitargs(line, "=,", &argc);
	if (!argv) { zc_error("Unbalanced quotes in configuration line"); goto err; }
	if (argc != 6) { zc_error("deepness must have 6 arguments[%d]", argc); goto err; }

	a_deepness->sign = zc_sdsnew(argv[0] + 1);
	if (!a_deepness->sign) { zc_error("zc_sdsnew fail, errno[%d]", errno); goto err;  }
	errno = 0;
        a_deepness->perm = (mode_t)strtol(argv[1] + 1, NULL, 8);
	if (errno || a_deepness->perm > 0777) { zc_error("Invalid file permissions"); goto err; }
	e = 0; a_deepness->buffer_len = zc_strtoz(argv[2] + 1, &e);
	if (e) { zc_error("zc_strtoz fail"); goto err; }
	e = 0; a_deepness->flush_len = zc_strtoz(argv[3] + 1, &e);
	if (e) { zc_error("zc_strtoz fail"); goto err; }
	e = 0; a_deepness->flush_count = zc_strtoz(argv[4] + 1, &e);
	if (e) { zc_error("zc_strtoz fail"); goto err; }
	e = 0; a_deepness->fsync_count = zc_strtoz(argv[5] + 1, &e);
	if (e) { zc_error("zc_strtoz fail"); goto err; }

	if (argv) zc_sdsfreesplitres(argv, argc);
	zlog_deepness_profile(a_deepness, ZC_DEBUG);
	return a_deepness;
err:
	if (argv) zc_sdsfreesplitres(argv, argc);
	zlog_deepness_del(a_deepness);
	return NULL;
}
