#ifndef _NEWFS_H_
#define _NEWFS_H_

#define FUSE_USE_VERSION 26
#include "stdio.h"
#include "stdlib.h"
#include <unistd.h>
#include "fcntl.h"
#include "string.h"
#include "fuse.h"
#include <stddef.h>
#include "ddriver.h"
#include "errno.h"
#include "types.h"
#include "stdint.h"

#define NEWFS_MAGIC           69     // 盘上超级块的幻数，用于识别文件系统


#define NEWFS_BLOCK_SIZE 	    1024 // 文件系统逻辑块大小，单位为字节
#define NEWFS_INODE_BLOCKS      33   // inode 区占用的逻辑块数
#define NEWFS_INODE_LIMIT       676  // 最多可分配的 inode 数量，包含根 inode
#define NEWFS_DIRECT_BLOCKS     6    // 每个 inode 最多直接引用的数据块数

// 每块只存放完整记录，块尾不足一条记录的空间不使用
#define NEWFS_INODES_PER_BLOCK  (NEWFS_BLOCK_SIZE / sizeof(struct newfs_inode_d))
#define NEWFS_DENTRIES_PER_BLOCK (NEWFS_BLOCK_SIZE / sizeof(struct newfs_dentry_d))

#define NEWFS_DEFAULT_PERM    0777   /* 全权限打开 */

/******************************************************************************
* SECTION: newfs.c
*******************************************************************************/
void* 			   newfs_init(struct fuse_conn_info *);
void  			   newfs_destroy(void *);
int   			   newfs_mkdir(const char *, mode_t);
int   			   newfs_getattr(const char *, struct stat *);
int   			   newfs_readdir(const char *, void *, fuse_fill_dir_t, off_t,
						                struct fuse_file_info *);
int   			   newfs_mknod(const char *, mode_t, dev_t);
int   			   newfs_write(const char *, const char *, size_t, off_t,
					                  struct fuse_file_info *);
int   			   newfs_read(const char *, char *, size_t, off_t,
					                 struct fuse_file_info *);
int   			   newfs_access(const char *, int);
int   			   newfs_unlink(const char *);
int   			   newfs_rmdir(const char *);
int   			   newfs_rename(const char *, const char *);
int                newfs_link(const char *, const char *);
int                newfs_symlink(const char *, const char *);
int                newfs_readlink(const char *, char *, size_t);
int   			   newfs_utimens(const char *, const struct timespec tv[2]);
int   			   newfs_truncate(const char *, off_t);
			
int   			   newfs_open(const char *, struct fuse_file_info *);
int   			   newfs_opendir(const char *, struct fuse_file_info *);

#endif  /* _newfs_H_ */
