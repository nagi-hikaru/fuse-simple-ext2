#define _XOPEN_SOURCE 700

#include "newfs.h"
#include "bonus.h"
#include <time.h>

/******************************************************************************
* SECTION: 宏定义
*******************************************************************************/
#define OPTION(t, p)        { t, offsetof(struct custom_options, p), 1 }

/******************************************************************************
* SECTION: 全局变量
*******************************************************************************/
static const struct fuse_opt option_spec[] = {		/* 用于FUSE文件系统解析参数 */
	OPTION("--device=%s", device),
	FUSE_OPT_END
};

struct custom_options newfs_options;			 /* 全局选项 */
struct newfs_super super = { .fd = -1 };

/******************************************************************************
* SECTION: FUSE操作定义
*******************************************************************************/
static struct fuse_operations operations = {
	.init = newfs_init,						 /* mount文件系统 */		
	.destroy = newfs_destroy,				 /* umount文件系统 */
	.mkdir = newfs_mkdir,					 /* 建目录，mkdir */
	.getattr = newfs_getattr,				 /* 获取文件属性，类似stat，必须完成 */
	.readdir = newfs_readdir,				 /* 填充dentrys */
	.mknod = newfs_mknod,					 /* 创建文件，touch相关 */
	.write = NULL,							 /* 写入文件 */
	.read = newfs_read,                      /* 附加题一：未完成时返回 ENOSYS */
	.utimens = newfs_utimens,				 /* 修改时间，忽略，避免touch报错 */
	.truncate = NULL,						 /* 改变文件大小 */
	.unlink = NULL,							 /* 删除文件 */
	.rmdir	= NULL,							 /* 删除目录， rm -r */
	.rename = NULL,							 /* 重命名，mv */

	.open = NULL,							
	.opendir = NULL,
	.access = NULL
};
/******************************************************************************
* SECTION: 辅助函数实现
*******************************************************************************/
// 从指定偏移 offset 读取 size 个字节到 buf 中
static int read_disk(off_t offset, void *buf, size_t size) {
    if (offset < 0 || offset > super.sz_disk || size > (size_t)(super.sz_disk - offset))
        return -EINVAL;
    if (size == 0)
        return 0;

    char temp[super.sz_io];
    char *out = buf;
    size_t remain = offset % super.sz_io;
    off_t start = offset - (off_t)remain;
    if (ddriver_seek(super.fd, start, SEEK_SET) != start)
        return -EIO;

    while (size > 0) {
        int result = ddriver_read(super.fd, temp, super.sz_io);
        if (result != super.sz_io)
            return result < 0 ? result : -EIO;

        size_t count = super.sz_io - remain;
        if (count > size) 
            count = size;
        memcpy(out, temp + remain, count);
        out += count;
        size -= count;
        remain = 0;
    }
    return 0;
}

// 将 buf 中 size 个字节写到指定偏移 offset 中，先读后写
static int write_disk(off_t offset, const void *buf, size_t size) {
    if (offset < 0 || offset > super.sz_disk || size > (size_t)(super.sz_disk - offset))
        return -EINVAL;
    if (size == 0)
        return 0;

    char temp[super.sz_io];
    const char *in = buf;
    while (size > 0) {
        size_t remain = offset % super.sz_io;
        off_t start = offset - (off_t)remain;
        size_t count = super.sz_io - remain;
        if (count > size)
            count = size;

        // 如果只改部分 io 块
        if (count < (size_t)super.sz_io) {
            int result = read_disk(start, temp, super.sz_io);
            if (result < 0)
                return result;
        }
        memcpy(temp + remain, in, count);
        if (ddriver_seek(super.fd, start, SEEK_SET) != start)
            return -EIO;

        int result = ddriver_write(super.fd, temp, super.sz_io);
        if (result != super.sz_io)
            return result < 0 ? result : -EIO;

        offset += (off_t)count;
        in += count;
        size -= count;
    }
    return 0;
}

// 读取指定 ino 的 inode，结果放在 *inode
static int read_inode(uint32_t ino, struct newfs_inode_d *inode) {
    int block_no = super.inode_offset + ino / NEWFS_INODES_PER_BLOCK;
    off_t offset = (off_t)block_no * NEWFS_BLOCK_SIZE
                  + (ino % NEWFS_INODES_PER_BLOCK) * sizeof(*inode);
    return read_disk(offset, inode, sizeof(*inode));
}

// 根据 inode 编号写入 inode 记录
static int write_inode(const struct newfs_inode_d *inode) {
    int block_no = super.inode_offset + inode->ino / NEWFS_INODES_PER_BLOCK;
    off_t offset = (off_t)block_no * NEWFS_BLOCK_SIZE
                  + (inode->ino % NEWFS_INODES_PER_BLOCK) * sizeof(*inode);
    return write_disk(offset, inode, sizeof(*inode));
}

// 递归加载目录树，并组织成内存 inode 结构
static int load_subtree(uint32_t ino, uint8_t *visited, struct newfs_inode **out) {
    if (ino >= (uint32_t)super.ino_max || visited[ino] ||
        !(super.ino_map[ino / 8] & (1u << (ino % 8))))
        return -EIO;
    visited[ino] = 1;

    // 读取对应 inode
    struct newfs_inode_d disk_inode;
    int result = read_inode(ino, &disk_inode);
    if (result < 0)
        return result;
    if (disk_inode.ino != ino ||
        (disk_inode.ftype != NEWFS_FILE && disk_inode.ftype != NEWFS_DIR) ||
        disk_inode.block_count > NEWFS_DIRECT_BLOCKS ||
        disk_inode.size > (uint32_t)super.file_max || disk_inode.dir_cnt < 0 ||
        (uint32_t)disk_inode.dir_cnt > disk_inode.block_count * NEWFS_DENTRIES_PER_BLOCK ||
        (disk_inode.ftype == NEWFS_FILE && disk_inode.dir_cnt != 0))
        return -EIO;

    for (uint32_t i = 0; i < disk_inode.block_count; i++) {
        int block = disk_inode.data_block[i];
        if (block < 0 || block >= super.data_blks ||
            !(super.data_map[block / 8] & (1u << (block % 8))))
            return -EIO;
    }

    // 创建内存 inode
    struct newfs_inode *inode = calloc(1, sizeof(*inode));
    if (!inode)
        return -ENOMEM;
    *out = inode;
    inode->ino = disk_inode.ino;
    inode->ftype = disk_inode.ftype;
    inode->dir_cnt = disk_inode.dir_cnt;
    inode->size = disk_inode.size;
    inode->block_count = disk_inode.block_count;
    memcpy(inode->data_block, disk_inode.data_block, sizeof(inode->data_block));

    int remaining = inode->dir_cnt;
    struct newfs_dentry **tail = &inode->children;
    for (uint32_t block = 0; remaining > 0; block++) {
        // 遍历数据块
        char buf[NEWFS_BLOCK_SIZE];
        off_t offset = (off_t)(super.data_offset + inode->data_block[block]) * NEWFS_BLOCK_SIZE;
        result = read_disk(offset, buf, sizeof(buf));
        if (result < 0)
            return result;

        // 遍历目录项
        for (int slot = 0; slot < NEWFS_DENTRIES_PER_BLOCK && remaining > 0; slot++, remaining--) {
            struct newfs_dentry_d entry;
            memcpy(&entry, buf + slot * sizeof(entry), sizeof(entry));
            if (!entry.file_name[0] ||
                !memchr(entry.file_name, '\0', sizeof(entry.file_name)) ||
                strchr(entry.file_name, '/') || !strcmp(entry.file_name, ".") ||
                !strcmp(entry.file_name, ".."))
                return -EIO;

            struct newfs_dentry *child = calloc(1, sizeof(*child));
            if (!child)
                return -ENOMEM;
            memcpy(child->name, entry.file_name, sizeof(child->name));
            child->ftype = entry.ftype;
            *tail = child;
            tail = &child->next;

            result = load_subtree(entry.ino, visited, &child->inode);
            if (result < 0)
                return result;
            if (child->ftype != child->inode->ftype)
                return -EIO;
        }
    }
    return 0;
}

// 加载目录树
static int load_tree(uint32_t ino, struct newfs_inode **out) {
    uint8_t visited[NEWFS_INODE_LIMIT] = {0};
    return load_subtree(ino, visited, out);
}

// 分配空闲逻辑块编号
static int alloc_bitmap(uint8_t *map, int limit) {
    for (int number = 0; number < limit; number++) {
        uint8_t bit = 1u << (number % 8);
        if (!(map[number / 8] & bit)) {
            map[number / 8] |= bit;
            return number;
        }
    }
    return -ENOSPC;
}

// 根据 path 在目录树中查找对应 inode
static int lookup(const char *path, struct newfs_inode **out) {
    if (!super.root_inode)
        return -EIO;
    if (!path || path[0] != '/')
        return -EINVAL;

    struct newfs_inode *inode = super.root_inode;
    const char *part = path + 1;
    while (*part) {
        if (inode->ftype != NEWFS_DIR)
            return -ENOTDIR;
        while (*part == '/')
            part++;
        if (!*part)
            break;
        size_t length = strcspn(part, "/");
        if (length >= MAX_NAME_LEN)
            return -ENAMETOOLONG;
        struct newfs_dentry *child = inode->children;
        while (child && (strlen(child->name) != length ||
                         memcmp(child->name, part, length) != 0))
            child = child->next;
        if (!child)
            return -ENOENT;
        inode = child->inode;
        part += length;
    }
    *out = inode;
    return 0;
}

// 创建目录或普通文件
static int create_node(const char *path, FILE_TYPE type) {
    if (!path || path[0] != '/')
        return -EINVAL;
    const char *name = strrchr(path, '/') + 1;
    size_t length = strlen(name);
    if (!length)
        return -EEXIST;
    if (length >= MAX_NAME_LEN)
        return -ENAMETOOLONG;
    if (!strcmp(name, ".") || !strcmp(name, ".."))
        return -EINVAL;

    // 获取 parent 的 inode
    char *parent_path = name == path + 1 ? strdup("/") : strndup(path, name - path - 1);
    if (!parent_path)
        return -ENOMEM;
    struct newfs_inode *parent;
    int result = lookup(parent_path, &parent);
    free(parent_path);
    if (result < 0)
        return result;
    if (parent->ftype != NEWFS_DIR)
        return -ENOTDIR;

    // 寻找是否已经存在
    struct newfs_dentry **tail = &parent->children;
    while (*tail) {
        if (!strcmp((*tail)->name, name))
            return -EEXIST;
        tail = &(*tail)->next;
    }
    if (parent->dir_cnt >= NEWFS_DIRECT_BLOCKS * NEWFS_DENTRIES_PER_BLOCK)
        return -ENOSPC;

    struct newfs_inode *inode = calloc(1, sizeof(*inode));
    struct newfs_dentry *entry = calloc(1, sizeof(*entry));
    if (!inode || !entry) {
        free(inode);
        free(entry);
        return -ENOMEM;
    }
    // 分配一个空闲 inode 编号
    int ino = alloc_bitmap(super.ino_map, super.ino_max);
    if (ino < 0) {
        free(inode);
        free(entry);
        return ino;
    }

    // 当前子项数为 0、7、14……时，插入新项需要再分配一块
    if (parent->dir_cnt % NEWFS_DENTRIES_PER_BLOCK == 0) {
        int block = alloc_bitmap(super.data_map, super.data_blks);
        if (block < 0) {
            super.ino_map[ino / 8] &= (uint8_t)~(1u << (ino % 8));
            free(inode);
            free(entry);
            return block;
        }
        parent->data_block[parent->block_count++] = block;
    }

    // 插入 entry 并绑定 inode
    inode->ino = (uint32_t)ino;
    inode->ftype = type;
    memcpy(entry->name, name, length + 1);
    entry->ftype = type;
    entry->inode = inode;
    *tail = entry;
    parent->dir_cnt++;
    parent->size = parent->dir_cnt * sizeof(struct newfs_dentry_d);
    return 0;
}

// 同步目录树
static int sync_tree(const struct newfs_inode *inode) {
    for (struct newfs_dentry *child = inode->children; child; child = child->next) {
        int result = sync_tree(child->inode);
        if (result < 0)
            return result;
    }

    // 写回目录项
    if (inode->ftype == NEWFS_DIR) {
        struct newfs_dentry *child = inode->children;
        for (uint32_t block = 0; block < inode->block_count; block++) {
            char buf[NEWFS_BLOCK_SIZE] = {0};
            for (int slot = 0; slot < NEWFS_DENTRIES_PER_BLOCK && child; slot++) {
                struct newfs_dentry_d entry = {
                    .ino = child->inode->ino,
                    .ftype = child->ftype
                };
                memcpy(entry.file_name, child->name, sizeof(entry.file_name));
                memcpy(buf + slot * sizeof(entry), &entry, sizeof(entry));
                child = child->next;
            }
            off_t offset = (off_t)(super.data_offset + inode->data_block[block]) * NEWFS_BLOCK_SIZE;
            int result = write_disk(offset, buf, sizeof(buf));
            if (result < 0)
                return result;
        }
    }

    // 写回 inode
    struct newfs_inode_d disk_inode = {
        .ino = inode->ino,
        .ftype = inode->ftype,
        .dir_cnt = inode->dir_cnt,
        .size = inode->size,
        .block_count = inode->block_count,
        .link = inode == super.root_inode ? 2 : 1
    };
    memcpy(disk_inode.data_block, inode->data_block, sizeof(disk_inode.data_block));
    return write_inode(&disk_inode);
}

// 递归释放内存中的 inode 和 dentry 树
static void free_tree(struct newfs_inode *inode) {
    if (!inode)
        return;
    struct newfs_dentry *child = inode->children;
    while (child) {
        struct newfs_dentry *next = child->next;
        free_tree(child->inode);
        free(child);
        child = next;
    }
    free(inode);
}

// 释放内存中的资源
static void release_mount(void) {
    free_tree(super.root_inode);
    if (super.fd >= 0)
        ddriver_close(super.fd);
    super = (struct newfs_super) { .fd = -1 };
}

/******************************************************************************
* SECTION: 必做函数实现
*******************************************************************************/
/**
 * @brief 挂载文件系统，初始化空盘或恢复已有目录树。
 *
 * @param conn_info FUSE 连接信息
 * @return void* 成功时返回内存超级块地址
 */
void *newfs_init(struct fuse_conn_info *conn_info) {
    int result;
    struct newfs_super_d disk_super;

    // 在内存保存 bitmap
    static uint8_t ino_map[NEWFS_BLOCK_SIZE];
    static uint8_t data_map[NEWFS_BLOCK_SIZE];
    super.ino_map = ino_map;
    super.data_map = data_map;

    // 打开驱动
    super.fd = ddriver_open((char *)newfs_options.device);
    if (super.fd < 0) {
        result = super.fd;
        goto fail;
    }

    // 查询驱动信息
    ddriver_ioctl(super.fd, IOC_REQ_DEVICE_SIZE, &super.sz_disk);
    ddriver_ioctl(super.fd, IOC_REQ_DEVICE_IO_SZ, &super.sz_io);
    super.blks_nums = super.sz_disk / NEWFS_BLOCK_SIZE;

    // 读取超级块
    result = read_disk(0, &disk_super, sizeof(disk_super));
    if (result < 0)
        goto fail;

    // 第一次挂载直接初始化内存中的超级块、位图和根目录
    if (disk_super.magic_number != NEWFS_MAGIC) {
        super.inode_offset = 3;
        super.inode_blks = NEWFS_INODE_BLOCKS;
        super.data_offset = 3 + NEWFS_INODE_BLOCKS;
        super.data_blks = super.blks_nums - super.data_offset;
        super.ino_max = NEWFS_INODE_LIMIT;
        super.file_max = NEWFS_DIRECT_BLOCKS * NEWFS_BLOCK_SIZE;

        memset(ino_map, 0, sizeof(ino_map));
        memset(data_map, 0, sizeof(data_map));

        super.root_inode = calloc(1, sizeof(*super.root_inode));
        if (!super.root_inode) {
            result = -ENOMEM;
            goto fail;
        }
        ino_map[0] = 1;
        super.root_inode->ino = 0;
        super.root_inode->ftype = NEWFS_DIR;
    } else {
        // 重挂时从盘上超级块恢复布局
        super.inode_offset = disk_super.inode_offset;
        super.inode_blks = disk_super.inode_blks;
        super.data_offset = disk_super.data_offset;
        super.data_blks = disk_super.data_blks;
        super.ino_max = disk_super.ino_max;
        super.file_max = disk_super.file_max;

        result = read_disk((off_t)disk_super.ino_map_offset * NEWFS_BLOCK_SIZE, ino_map, sizeof(ino_map));
        if (result < 0)
            goto fail;
        result = read_disk((off_t)disk_super.data_map_offset * NEWFS_BLOCK_SIZE, data_map, sizeof(data_map));
        if (result < 0)
            goto fail;
        result = load_tree(disk_super.root_ino, &super.root_inode);
        if (result < 0)
            goto fail;
    }

    return &super;

fail:
    fprintf(stderr, "newfs: mount failed: %s\n", strerror(-result));
    release_mount();
    if (conn_info) {
        struct fuse_context *context = fuse_get_context();
        if (context && context->fuse)
            fuse_exit(context->fuse);
    }
    return NULL;
}

/**
 * @brief 卸载文件系统，写回目录树、位图和超级块，再释放运行时资源。
 * @param p FUSE 私有数据
 */
void newfs_destroy(void *p) {
    (void)p;
    int result = 0;
    if (super.root_inode) {
        struct newfs_super_d disk_super = {
            .magic_number = NEWFS_MAGIC,
            .ino_map_offset = 1,
            .ino_map_blks = 1,
            .data_map_offset = 2,
            .data_map_blks = 1,
            .inode_offset = super.inode_offset,
            .inode_blks = super.inode_blks,
            .data_offset = super.data_offset,
            .data_blks = super.data_blks,
            .ino_max = super.ino_max,
            .file_max = super.file_max,
            .root_ino = super.root_inode->ino
        };

        // 写回超级块
        result = write_disk(0, &disk_super, sizeof(disk_super));
        // 写回 inode 位图
        if (result == 0)
            result = write_disk((off_t)disk_super.ino_map_offset * NEWFS_BLOCK_SIZE, super.ino_map, NEWFS_BLOCK_SIZE);
        // 写回 data 位图
        if (result == 0)
            result = write_disk((off_t)disk_super.data_map_offset * NEWFS_BLOCK_SIZE, super.data_map, NEWFS_BLOCK_SIZE);
        // 写回文件树
        if (result == 0) 
            result = sync_tree(super.root_inode);
        // 让操作系统将缓存同步到底层存储
        if (result == 0 && fsync(super.fd) < 0)
            result = -errno;
    }
    if (result < 0)
        fprintf(stderr, "newfs: unmount sync failed: %s\n", strerror(-result));
    release_mount();
}

/**
 * @brief 创建目录，分配 inode 并将目录项加入父目录。
 * @param path 相对于文件系统根目录的绝对路径
 * @param mode 目录权限，统一使用默认权限
 * @return int 成功返回 0，失败返回负错误码
 */
int newfs_mkdir(const char *path, mode_t mode) {
    (void)mode;
    int result = create_node(path, NEWFS_DIR);
    return result;
}

/**
 * @brief 创建普通空文件，分配 inode 并将目录项加入父目录。
 * @param path 相对于文件系统根目录的绝对路径
 * @param mode 文件类型与权限
 * @param dev 设备编号，忽略
 * @return int 成功返回 0，失败返回负错误码
 */
int newfs_mknod(const char *path, mode_t mode, dev_t dev) {
    (void)dev;
    if ((mode & S_IFMT) && !S_ISREG(mode))
        return -EINVAL;
    int result = create_node(path, NEWFS_FILE);
    return result;
}

/**
 * @brief 查找文件或目录，读取 inode 并填充文件属性。
 * @param path 相对于文件系统根目录的绝对路径
 * @param newfs_stat 接收文件类型、大小、权限等属性的结构体
 * @return int 成功返回 0，失败返回负错误码
 */
int newfs_getattr(const char *path, struct stat *stat) {
    struct newfs_inode *inode;
    int result = lookup(path, &inode);
    if (result == 0) {
        memset(stat, 0, sizeof(*stat));
        stat->st_ino = inode->ino;
        stat->st_mode = (inode->ftype == NEWFS_DIR ? S_IFDIR : S_IFREG) | NEWFS_DEFAULT_PERM;
        stat->st_nlink = inode == super.root_inode ? 2 : 1;
        stat->st_uid = getuid();
        stat->st_gid = getgid();
        stat->st_size = inode->size;
        stat->st_blksize = NEWFS_BLOCK_SIZE;
        stat->st_blocks = inode->block_count * (NEWFS_BLOCK_SIZE / super.sz_io);
        stat->st_atime = stat->st_mtime = stat->st_ctime = time(NULL);
    }
    return result;
}

/**
 * @brief 遍历目录的子项，调用 filler 将文件名填入结果缓冲区。
 * @param path 相对于文件系统根目录的绝对路径
 * @param buf FUSE 提供的目录结果缓冲区
 * @param filler FUSE 提供的目录项填充函数
 * @param offset 本次开始读取的目录项序号，从 0 开始
 * @param fi 打开的目录信息，忽略
 * @return int 成功返回 0，失败返回负错误码
 */
int newfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
                  struct fuse_file_info *fi) {
    (void)fi;
    if (offset < 0)
        return -EINVAL;
    struct newfs_inode *inode;
    int result = lookup(path, &inode);
    if (result == 0 && inode->ftype != NEWFS_DIR)
        result = -ENOTDIR;
    if (result == 0) {
        off_t index = 0;
        for (struct newfs_dentry *child = inode->children; child; child = child->next, index++) {
            if (index >= offset && filler(buf, child->name, NULL, index + 1))
                break;
        }
    }
    return result;
}

/**
 * @brief 处理 touch 可能触发的时间更新请求，目前仅检查路径是否存在，不更新时间戳。
 * @param path 相对于文件系统根目录的绝对路径
 * @param tv 访问时间与修改时间，当前实现忽略
 * @return int 路径存在返回 0，失败返回负错误码
 */
int newfs_utimens(const char *path, const struct timespec tv[2]) {
    (void)tv;
    struct newfs_inode *inode;
    int result = lookup(path, &inode);
    return result;
}

/******************************************************************************
* SECTION: 选做函数实现
*******************************************************************************/
/**
 * @brief 写入文件
 * 
 * @param path 相对于挂载点的路径
 * @param buf 写入的内容
 * @param size 写入的字节数
 * @param offset 相对文件的偏移
 * @param fi 可忽略
 * @return int 写入大小
 */
int newfs_write(const char* path, const char* buf, size_t size, off_t offset,
		        struct fuse_file_info* fi) {
	/* 选做 */
	return size;
}

/**
 * @brief 读取文件
 * 
 * @param path 相对于挂载点的路径
 * @param buf 读取的内容
 * @param size 读取的字节数
 * @param offset 相对文件的偏移
 * @param fi 可忽略
 * @return int 读取大小
 */
int newfs_read(const char* path, char* buf, size_t size, off_t offset,
		       struct fuse_file_info* fi) {
	/* 附加题一：完成普通文件读取，供 FUSE 和附加题复用。 */
	return -ENOSYS;
}

/**
 * @brief 删除文件
 * 
 * @param path 相对于挂载点的路径
 * @return int 0成功，否则返回对应错误号
 */
int newfs_unlink(const char* path) {
	/* 选做 */
	return 0;
}

/**
 * @brief 删除目录
 * 
 * 一个可能的删除目录操作如下：
 * rm ./tests/mnt/j/ -r
 *  1) Step 1. rm ./tests/mnt/j/j
 *  2) Step 2. rm ./tests/mnt/j
 * 即，先删除最深层的文件，再删除目录文件本身
 * 
 * @param path 相对于挂载点的路径
 * @return int 0成功，否则返回对应错误号
 */
int newfs_rmdir(const char* path) {
	/* 选做 */
	return 0;
}

/**
 * @brief 重命名文件 
 * 
 * @param from 源文件路径
 * @param to 目标文件路径
 * @return int 0成功，否则返回对应错误号
 */
int newfs_rename(const char* from, const char* to) {
	/* 选做 */
	return 0;
}

/**
 * @brief 打开文件，可以在这里维护fi的信息，例如，fi->fh可以理解为一个64位指针，可以把自己想保存的数据结构
 * 保存在fh中
 * 
 * @param path 相对于挂载点的路径
 * @param fi 文件信息
 * @return int 0成功，否则返回对应错误号
 */
int newfs_open(const char* path, struct fuse_file_info* fi) {
	/* 选做 */
	return 0;
}

/**
 * @brief 打开目录文件
 * 
 * @param path 相对于挂载点的路径
 * @param fi 文件信息
 * @return int 0成功，否则返回对应错误号
 */
int newfs_opendir(const char* path, struct fuse_file_info* fi) {
	/* 选做 */
	return 0;
}

/**
 * @brief 改变文件大小
 * 
 * @param path 相对于挂载点的路径
 * @param offset 改变后文件大小
 * @return int 0成功，否则返回对应错误号
 */
int newfs_truncate(const char* path, off_t offset) {
	/* 选做 */
	return 0;
}


/**
 * @brief 访问文件，因为读写文件时需要查看权限
 * 
 * @param path 相对于挂载点的路径
 * @param type 访问类别
 * R_OK: Test for read permission. 
 * W_OK: Test for write permission.
 * X_OK: Test for execute permission.
 * F_OK: Test for existence. 
 * 
 * @return int 0成功，否则返回对应错误号
 */
int newfs_access(const char* path, int type) {
	/* 选做: 解析路径，判断是否存在 */
	return 0;
}	
/******************************************************************************
* SECTION: FUSE入口
*******************************************************************************/
int main(int argc, char **argv)
{
    int ret;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	/* 未指定 --device 时由驱动使用当前用户的默认磁盘。 */

	if (fuse_opt_parse(&args, &newfs_options, option_spec, NULL) == -1)
		return -1;

	/* 默认使用单线程，由 FUSE 串行调用文件系统回调。 */
	if (fuse_opt_insert_arg(&args, 1, "-s") == -1) {
		fuse_opt_free_args(&args);
		return 1;
	}
	
	/* 教师已接好附加题分发；普通路径仍调用上面的学生回调。 */
	ret = bonus_register(&operations);
	if (ret < 0) {
		fprintf(stderr, "Cannot register bonus callbacks: %s\n", strerror(-ret));
		fuse_opt_free_args(&args);
		return 1;
	}

	ret = fuse_main(args.argc, args.argv, &operations, NULL);
	fuse_opt_free_args(&args);
	return ret;
}
