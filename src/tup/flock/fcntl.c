/* vim: set ts=8 sw=8 sts=8 noet tw=78:
 *
 * tup - A file-based build system
 *
 * Copyright (C) 2011-2026  Mike Shal <marfey@gmail.com>
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

#define _ATFILE_SOURCE
#include "tup/flock.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#ifdef USE_DOTLOCK

#include "../bsd/tree.h"

#include <sys/socket.h>
#include <sys/un.h>

#include <sys/stat.h>
#include <stdlib.h>
#include <stddef.h>
#include <threads.h>

// --- helper functions ---

static int get_lockname(ino_t inode, char *name, int len)
{
	int ret = snprintf(name, len, "tup-%lu", inode);
	if (ret >= 0 && ret < len) return ret;
	return -1;
}


// --- locking ---

static mtx_t inode_tree_mtx;

#define dotlock_path_len 64
// return >0: lock descriptor (lock optained)
//	 -1: lock not optained (if errono is EADDRINUSE, lock is held by another process)
static int get_lock(ino_t inode)
{
	char name[dotlock_path_len];
	struct sockaddr_un addr;

	int nlen = get_lockname(inode, name, sizeof(name));
	if (nlen < 0 || nlen > (int)sizeof(addr.sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	addr.sun_path[0] = '\0'; // Abstract namespace socket: leading '\0'
	memcpy(addr.sun_path + 1, name, nlen);
	int alen = offsetof(struct sockaddr_un, sun_path) + 1 + nlen;


	int sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		return -1;
	}

	if (bind(sock, (struct sockaddr*)&addr, alen) < 0) {
		int err = errno;
		close(sock);
		errno = err;
		return -1;
	}

	// Lock acquired
	return sock;
}

// --- splay tree ---

struct node {
	SPLAY_ENTRY(node) link;   // intrusive field — name can be anything
	ino_t inode;
	int fd_lock;
	pid_t pid;
};

static inline int node_cmp(struct node *a, struct node *b)
{
	if (a->pid == b->pid) return (a->inode < b->inode) ? -1 : (a->inode > b->inode) ?  1 : 0;
	else return (a->pid < b->pid) ? -1 : (a->pid > b->pid) ?  1 : 0;
}

SPLAY_HEAD(inode_tree, node) inode_tree_head = SPLAY_INITIALIZER(&inode_tree_head);
SPLAY_PROTOTYPE(inode_tree, node, link, node_cmp)
SPLAY_GENERATE(inode_tree, node, link, node_cmp)

/*
static void inode_tree_dump(void)
{
	fprintf(stderr, "---------- pid=%d\n", getpid());
	for (struct node *it = SPLAY_MIN(inode_tree, &inode_tree_head); it != NULL; it = SPLAY_NEXT(inode_tree, &inode_tree_head, it)) {
		fprintf(stderr, "inode=%lu, lock=%d, pid=%d\n", it->inode, it->fd_lock, it->pid);
	}
	if (SPLAY_MIN(inode_tree, &inode_tree_head) == NULL) {
		fprintf(stderr, "(tree is empty)\n");
	}
	fprintf(stderr, "----------\n");
}
*/

static int add_node(ino_t inode, int lock)
{
	struct node *n = malloc(sizeof(*n));
	if (!n) return -1;
	n->inode = inode;
	n->fd_lock = lock;
	n->pid = getpid();

	mtx_lock(&inode_tree_mtx);
        struct node *res = SPLAY_INSERT(inode_tree, &inode_tree_head, n);
	mtx_unlock(&inode_tree_mtx);
        if (res != NULL) {
		// already exists
		free(n);
		//inode_tree_dump();
		return -1;
	}

	return 0;
}

// return inode or 0 on error (inode 0 is never used)
static ino_t get_inode(int fd)
{
	struct stat file_stat;
	int result = fstat (fd, &file_stat);
	if (result < 0) return 0;
	return file_stat.st_ino;
}

static int remove_dotlock(int fd)
{
	int prev_errno = errno;

	struct node key;
	key.inode = get_inode(fd);
	key.pid = getpid();
	if (key.inode == 0) {
		errno = EIO;
		//inode_tree_dump();
		return -1;
	}

	mtx_lock(&inode_tree_mtx);
	struct node* n = SPLAY_FIND(inode_tree, &inode_tree_head, &key);
	if (n != NULL) n = SPLAY_REMOVE(inode_tree, &inode_tree_head, n);
	mtx_unlock(&inode_tree_mtx);
	if (n == NULL) {
		errno = EIO;
		//inode_tree_dump();
		return -1;
	}

	int ret = close(n->fd_lock);
	free(n);
	if (ret != 0)
	{
		errno = EIO;
		//inode_tree_dump();
		return -1;
	}
	errno = prev_errno;
	return 0;
}

/* Returns: -1 error, 0 got lock, 1 would block
 * wait_type: 0=don't wait, 1=F_SETLKW type, 2=wait_flock type */
static int make_dotlock(int fd, int wait_type)
{
	int prev_errno = errno;
	int wait = 0;
	while (1) {
		errno = 0; // Clear errors from previous loops

		ino_t inode = get_inode(fd);
		if (inode == 0) {
			//inode_tree_dump();
			errno = EIO;
			return -1;
		}

		int lock = get_lock(inode);
		if (lock >= 0) {
			// Got lock. Store it for later release
			int ret = add_node(inode, lock);
			if (ret < 0) {
				close(lock);
				errno = EIO;
				//inode_tree_dump();
				return -1;
			}
		}

		// type is don't wait
		if (wait_type == 0) {
			if (lock >= 0) {
				errno = prev_errno;
				return 0;
			}
			else if (errno == EADDRINUSE) {
				errno = EAGAIN;
				return 1;
			}
			else {
				errno = EIO;
				//inode_tree_dump();
				return -1;
			}
		}

		// type is F_SETLKW
		else if (wait_type == 1) {
			if (lock >= 0) {
				errno = prev_errno;
				return 0;
			}
			else if (errno == EADDRINUSE) {
				if (wait < 1000000) wait += 5000;
				usleep(wait);
			}
			else {
				errno = EIO;
				//inode_tree_dump();
				return -1;
			}
		}

		// type is wait_flock
		else if (wait_type == 2) {
			if (lock >= 0) {
				errno = prev_errno;
				return remove_dotlock(fd);
			}
			else if (errno == EADDRINUSE) {
				usleep(10000);
			}
			else {
				errno = EIO;
				//inode_tree_dump();
				return -1;
			}
		}

	}
	//inode_tree_dump();
	return -6;
}



#endif

int tup_lock_open(int basefd, const char *lockname, tup_lock_t *lock)
{
	int fd;

	fd = openat(basefd, lockname, O_RDWR | O_CREAT, 0666);
	if(fd < 0) {
		perror(lockname);
		fprintf(stderr, "tup error: Unable to open lockfile.\n");
		return -1;
	}
	*lock = fd;
	
	return 0;
}

void tup_lock_close(tup_lock_t lock)
{
	if(close(lock) < 0) {
		perror("close(lock)");
	}
}

int tup_flock(tup_lock_t fd)
{
#ifdef USE_DOTLOCK
	return make_dotlock(fd, 1);
#else
	struct flock fl = {
		.l_type = F_WRLCK,
		.l_whence = SEEK_SET,
		.l_start = 0,
		.l_len = 0,
	};

	if(fcntl(fd, F_SETLKW, &fl) < 0) {
		perror("fcntl F_WRLCK");
		return -1;
	}
#endif
	return 0;
}

/* Returns: -1 error, 0 got lock, 1 would block */
int tup_try_flock(tup_lock_t fd)
{
#ifdef USE_DOTLOCK
	return make_dotlock(fd, 0);
#else
	struct flock fl = {
		.l_type = F_WRLCK,
		.l_whence = SEEK_SET,
		.l_start = 0,
		.l_len = 0,
	};

	if(fcntl(fd, F_SETLK, &fl) < 0) {
		if (errno == EAGAIN)
			return 1;
		perror("fcntl F_WRLCK");
		return -1;
	}
#endif
	return 0;
}

int tup_unflock(tup_lock_t fd)
{
#ifdef USE_DOTLOCK
	if (remove_dotlock(fd) < 0) {
		perror("rm dotlock");
		return -1;
	}
#else
	struct flock fl = {
		.l_type = F_UNLCK,
		.l_whence = SEEK_SET,
		.l_start = 0,
		.l_len = 0,
	};

	if(fcntl(fd, F_SETLKW, &fl) < 0) {
		perror("fcntl F_UNLCK");
		return -1;
	}
#endif
	return 0;
}

int tup_wait_flock(tup_lock_t fd)
{
#ifdef USE_DOTLOCK
	return make_dotlock(fd, 2);
#else
	struct flock fl;

	while(1) {
		fl.l_type = F_WRLCK;
		fl.l_whence = SEEK_SET;
		fl.l_start = 0;
		fl.l_len = 0;

		if(fcntl(fd, F_GETLK, &fl) < 0) {
			perror("fcntl F_GETLK");
			return -1;
		}

		if(fl.l_type == F_WRLCK)
			break;
		usleep(10000);
	}
#endif
	return 0;
}
