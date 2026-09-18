/* vim: set ts=8 sw=8 sts=8 noet tw=78:
 *
 * tup - A file-based build system
 *
 * Copyright (C) 2009-2026  Mike Shal <marfey@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "bin.h"
#include "jp_alloc/jp_alloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct jp_pool_config bin_pool = JP_POOL_CONFIG(struct bin);
static const struct jp_pool_config bin_entry_pool = JP_POOL_CONFIG(struct bin_entry);

void bin_list_del(struct bin_head *head)
{
	while(!LIST_EMPTY(head)) {
		struct bin *b = LIST_FIRST(head);

		while(!TAILQ_EMPTY(&b->entries)) {
			struct bin_entry *be = TAILQ_FIRST(&b->entries);
			TAILQ_REMOVE(&b->entries, be, list);
			free(be->path);
			jp_pool_free(&bin_entry_pool, be);
		}
		LIST_REMOVE(b, list);
		free(b->name);
		jp_pool_free(&bin_pool, b);
	}
}

struct bin *bin_add(const char *name, struct bin_head *head)
{
	struct bin *b;
	int name_len;

	b = bin_find(name, head);
	if(b)
		return b;

	b = jp_pool_alloc(&bin_pool);
	if(!b) {
		perror("malloc");
		return NULL;
	}

	name_len = strlen(name);
	b->name = malloc(name_len + 1);
	if (!b->name) {
		perror("malloc");
		jp_pool_free(&bin_pool, b);
		return NULL;
	}
	memcpy(b->name, name, name_len);
	b->name[name_len] = 0;

	TAILQ_INIT(&b->entries);
	LIST_INSERT_HEAD(head, b, list);

	return b;
}

struct bin *bin_find(const char *name, struct bin_head *head)
{
	struct bin *b;

	LIST_FOREACH(b, head, list) {
		if(strcmp(b->name, name) == 0) {
			return b;
		}
	}
	return NULL;
}

int bin_add_entry(struct bin *b, const char *path, int len,
		  struct tup_entry *tent)
{
	struct bin_entry *be;

	be = jp_pool_alloc(&bin_entry_pool);
	if(!be) {
		perror("malloc");
		return -1;
	}
	be->path = malloc(len + 1);
	if(!be->path) {
		perror("malloc");
		jp_pool_free(&bin_entry_pool, be);
		return -1;
	}
	memcpy(be->path, path, len);
	be->path[len] = 0;
	be->len = len;

	be->tent = tent;
	TAILQ_INSERT_TAIL(&b->entries, be, list);
	return 0;
}
