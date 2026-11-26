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
struct newfs_super super = { .fd = -1 };         /* 内存超级块，fd 为 -1 表示设备未打开 */

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
	.write = newfs_write,					 /* 写入文件 */
	.read = newfs_read,					     /* 读取文件 */
	.utimens = newfs_utimens,				 /* 修改时间，忽略，避免touch报错 */
	.truncate = newfs_truncate,				 /* 改变文件大小 */
	.unlink = newfs_unlink,					 /* 删除文件 */
	.rmdir = newfs_rmdir,					 /* 删除空目录 */
	.rename = newfs_rename,					 /* 重命名，mv */
	.link = newfs_link,                      /* 硬链接，ln */
	.symlink = newfs_symlink,                /* 符号链接，ln -s */
	.readlink = newfs_readlink,              /* 读取符号链接目标 */

	.open = newfs_open,                       /* 打开普通文件，检查路径和类型 */
	.opendir = newfs_opendir,                 /* 打开目录，子项仍按需加载 */
	.access = newfs_access                    /* 检查路径及访问权限 */
};
/******************************************************************************
* SECTION: 辅助函数实现
*******************************************************************************/
// 从磁盘的字节偏移 offset 读取 size 个字节到 buf 中，不要求调用参数按 IO 大小对齐
static int read_disk(off_t offset, void *buf, size_t size) {
    if (offset < 0 || offset > super.sz_disk || size > (size_t)(super.sz_disk - offset))
        return -EINVAL;
    if (size == 0)
        return 0;

    // 驱动按 IO 大小读取，先定位到包含 offset 的 IO 块起点
    char temp[super.sz_io];
    char *out = buf;
    size_t remain = offset % super.sz_io; // 首个 IO 块内需要跳过的字节数
    off_t start = offset - (off_t)remain;
    if (ddriver_seek(super.fd, start, SEEK_SET) != start)
        return -EIO;

    while (size > 0) {
        int result = ddriver_read(super.fd, temp, super.sz_io);
        if (result != super.sz_io)
            return result < 0 ? result : -EIO;

        // 只复制本次需要的部分，首尾 IO 块可能只有部分字节属于请求范围
        size_t count = super.sz_io - remain;
        if (count > size) 
            count = size;
        memcpy(out, temp + remain, count);
        out += count;
        size -= count;
        remain = 0; // 后续 IO 块均从块头开始复制
    }
    return 0;
}

// 将 buf 中 size 个字节写到磁盘的字节偏移 offset，部分 IO 块先读后改再写回
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

        // 只覆盖部分 IO 块时，先读出旧内容以保留未修改字节；整块覆盖时无需读取
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
    // 商确定 inode 所在的逻辑块，余数确定块内第几条记录
    int block_no = super.inode_offset + ino / NEWFS_INODES_PER_BLOCK;
    off_t offset = (off_t)block_no * NEWFS_BLOCK_SIZE
                  + (ino % NEWFS_INODES_PER_BLOCK) * sizeof(*inode);
    return read_disk(offset, inode, sizeof(*inode));
}

// 根据 inode 编号写入 inode 记录
static int write_inode(const struct newfs_inode_d *inode) {
    // 每条记录都完整放在同一个逻辑块中，块尾剩余空间不参与编号
    int block_no = super.inode_offset + inode->ino / NEWFS_INODES_PER_BLOCK;
    off_t offset = (off_t)block_no * NEWFS_BLOCK_SIZE
                  + (inode->ino % NEWFS_INODES_PER_BLOCK) * sizeof(*inode);
    return write_disk(offset, inode, sizeof(*inode));
}

// 按需加载普通文件或符号链接的一块内容，block 为文件内从 0 开始的块序号
static int load_file_block(struct newfs_inode *inode, uint32_t block) {
    if (inode->data[block])
        return 0;
    // 先准备全零缓存，文件末尾以外的部分保持为零
    char *buf = calloc(1, NEWFS_BLOCK_SIZE);
    if (!buf)
        return -ENOMEM;

    // start 是本块在文件内的起始字节偏移，只读取当前文件大小以内的有效内容
    uint32_t start = block * NEWFS_BLOCK_SIZE;
    if (start < inode->size) {
        size_t count = inode->size - start;
        if (count > NEWFS_BLOCK_SIZE)
            count = NEWFS_BLOCK_SIZE;
        // 相对数据块号加上数据区起点，再换算为磁盘字节偏移
        off_t offset = (off_t)(super.data_offset + inode->data_block[block]) * NEWFS_BLOCK_SIZE;
        int result = read_disk(offset, buf, count);
        if (result < 0) {
            free(buf);
            return result;
        }
    }
    inode->data[block] = buf;
    return 0;
}

// 释放一个 inode 自身、它的目录项和数据缓存，不修改盘上分配位图
static void free_inode_memory(struct newfs_inode *inode) {
    if (!inode)
        return;
    // 子项引用的 inode 由 inode_cache 统一管理，不能沿目录树重复释放
    struct newfs_dentry *child = inode->children;
    while (child) {
        struct newfs_dentry *next = child->next;
        free(child);
        child = next;
    }
    for (int i = 0; i < NEWFS_DIRECT_BLOCKS; i++)
        free(inode->data[i]);
    free(inode);
}

// 加载指定编号的 inode 元数据，通过 out 返回共享对象，不加载子目录项或文件内容
static int load_inode(uint32_t ino, struct newfs_inode **out) {
    if (ino >= (uint32_t)super.ino_max ||
        !(super.ino_map[ino / 8] & (1u << (ino % 8))))
        return -EIO;
    // 同一编号始终返回同一对象，使多个硬链接看到相同内容和链接计数
    if (super.inode_cache[ino]) {
        *out = super.inode_cache[ino];
        return 0;
    }

    // 读取对应 inode
    struct newfs_inode_d disk_inode;
    int result = read_inode(ino, &disk_inode);
    if (result < 0)
        return result;
    // 检查类型、链接计数和记录容量，避免后续访问超出块数组或有效内容范围
    if (disk_inode.ino != ino ||
        (disk_inode.ftype != NEWFS_FILE && disk_inode.ftype != NEWFS_DIR &&
         disk_inode.ftype != NEWFS_SYMLINK) || disk_inode.link == 0 ||
        disk_inode.block_count > NEWFS_DIRECT_BLOCKS ||
        disk_inode.size > (uint32_t)super.file_max || disk_inode.dir_cnt < 0 ||
        (uint32_t)disk_inode.dir_cnt > disk_inode.block_count * NEWFS_DENTRIES_PER_BLOCK ||
        (disk_inode.ftype != NEWFS_DIR &&
         (disk_inode.dir_cnt != 0 ||
          disk_inode.size > disk_inode.block_count * NEWFS_BLOCK_SIZE)))
        return -EIO;

    // 每个有效数据块号都必须位于数据区内，且对应位图已经标记占用
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
    inode->link = disk_inode.link;
    inode->ftype = disk_inode.ftype;
    inode->dir_cnt = disk_inode.dir_cnt;
    inode->size = disk_inode.size;
    inode->block_count = disk_inode.block_count;
    memcpy(inode->data_block, disk_inode.data_block, sizeof(inode->data_block));
    // 放入编号缓存，后续目录项直接复用本对象
    super.inode_cache[ino] = inode;
    return 0;
}

// 加载本目录的子项和它们的 inode 元数据
static int load_directory(struct newfs_inode *inode) {
    if (inode->ftype != NEWFS_DIR)
        return -ENOTDIR;
    // 已加载的空目录 children 也是 NULL，必须用单独的标志判断是否需要读盘
    if (inode->children_loaded)
        return 0;

    int result;
    struct newfs_dentry *children = NULL;
    int remaining = inode->dir_cnt;
    struct newfs_dentry **tail = &children; // 指向下一个目录项应写入的指针位置
    for (uint32_t block = 0; remaining > 0; block++) {
        // 遍历数据块
        char buf[NEWFS_BLOCK_SIZE];
        off_t offset = (off_t)(super.data_offset + inode->data_block[block]) * NEWFS_BLOCK_SIZE;
        result = read_disk(offset, buf, sizeof(buf));
        if (result < 0)
            goto fail;

        // 遍历目录项
        for (int slot = 0; slot < NEWFS_DENTRIES_PER_BLOCK && remaining > 0; slot++, remaining--) {
            struct newfs_dentry_d entry;
            memcpy(&entry, buf + slot * sizeof(entry), sizeof(entry));
            // 盘上名字必须非空、有结束符，不包含斜杠，也不能是 . 或 ..
            if (!entry.file_name[0] ||
                !memchr(entry.file_name, '\0', sizeof(entry.file_name)) ||
                strchr(entry.file_name, '/') || !strcmp(entry.file_name, ".") ||
                !strcmp(entry.file_name, "..")) {
                result = -EIO;
                goto fail;
            }
            // 子项不能指向本目录或祖先目录，避免盘上目录循环
            for (struct newfs_inode *parent = inode; parent; parent = parent->parent) {
                if (entry.ino == parent->ino) {
                    result = -EIO;
                    goto fail;
                }
            }

            struct newfs_dentry *child = calloc(1, sizeof(*child));
            if (!child) {
                result = -ENOMEM;
                goto fail;
            }
            memcpy(child->name, entry.file_name, sizeof(child->name));
            child->ftype = entry.ftype;
            *tail = child;
            tail = &child->next;

            // 只加载子项的 inode 元数据，其子目录和文件内容留到实际访问时再读
            result = load_inode(entry.ino, &child->inode);
            if (result < 0)
                goto fail;
            if (child->ftype != child->inode->ftype) {
                result = -EIO;
                goto fail;
            }
            // 不允许目录有硬链接：既不能出现在不同父目录，也不能在本目录出现两次
            if (child->ftype == NEWFS_DIR) {
                if (child->inode->parent && child->inode->parent != inode) {
                    result = -EIO;
                    goto fail;
                }
                for (struct newfs_dentry *other = children; other != child; other = other->next) {
                    if (other->inode == child->inode) {
                        result = -EIO;
                        goto fail;
                    }
                }
                child->inode->parent = inode;
            }
        }
    }
    // 全部成功后再挂到 inode，避免其他操作看到只加载了一半的目录
    inode->children = children;
    inode->children_loaded = 1;
    return 0;

fail:
    // 完整加载成功后才发布链表，失败时允许下一次访问重新加载
    // 这里只释放临时目录项，已经加载的 inode 继续由缓存持有
    while (children) {
        struct newfs_dentry *next = children->next;
        free(children);
        children = next;
    }
    return result;
}

// 从位图中分配第一个空闲编号，既可用于 inode 编号，也可用于数据区相对块号
static int alloc_bitmap(uint8_t *map, int limit) {
    for (int number = 0; number < limit; number++) {
        uint8_t bit = 1u << (number % 8); // 编号除以 8 定位字节，取余定位字节内的位
        if (!(map[number / 8] & bit)) {
            map[number / 8] |= bit;
            return number;
        }
    }
    return -ENOSPC;
}

// 调整普通文件或符号链接的大小，调用方须检查类型和容量；分配失败时撤销新增资源
static int resize_file(struct newfs_inode *inode, uint32_t size) {
    uint32_t old_size = inode->size;
    uint32_t old_blocks = inode->block_count;
    uint32_t needed_blocks = (size + NEWFS_BLOCK_SIZE - 1) / NEWFS_BLOCK_SIZE; // 向上取整

    char *new_data[NEWFS_DIRECT_BLOCKS] = {0};
    uint32_t tail = old_size / NEWFS_BLOCK_SIZE; // 旧文件末尾不足一块时，该尾块的序号
    int needs_tail = size > old_size && old_size % NEWFS_BLOCK_SIZE != 0;
    int tail_loaded = 0;  // 记录是否是本次加载的缓存，失败路径使用
    int result;

    // 先申请空闲编号，空间不足时无需为新块分配缓存
    while (inode->block_count < needed_blocks) {
        int block = alloc_bitmap(super.data_map, super.data_blks);
        if (block < 0) {
            result = block;
            goto fail;
        }
        inode->data_block[inode->block_count++] = block;
    }

    // 编号申请成功后，再加载需要处理的旧尾块
    if (needs_tail && !inode->data[tail]) {
        result = load_file_block(inode, tail);
        if (result < 0)
            goto fail;
        tail_loaded = 1;
    }

    // 再准备新块的全零缓存，分配全部成功后再修改文件内容和大小
    for (uint32_t block = old_blocks; block < needed_blocks; block++) {
        if (!inode->data[block]) {
            new_data[block] = calloc(1, NEWFS_BLOCK_SIZE);
            if (!new_data[block]) {
                result = -ENOMEM;
                goto fail;
            }
        }
    }
    for (uint32_t block = old_blocks; block < needed_blocks; block++) {
        if (new_data[block])
            inode->data[block] = new_data[block];
        inode->dirty[block] = 1;
    }

    // 缩小时只释放超出新大小的整块，保留尾块暂不加载或清零
    while (inode->block_count > needed_blocks) {
        uint32_t slot = --inode->block_count;
        int block = inode->data_block[slot];
        super.data_map[block / 8] &= (uint8_t)~(1u << (block % 8));
        inode->data_block[slot] = 0;
        free(inode->data[slot]);
        inode->data[slot] = NULL;
        inode->dirty[slot] = 0;
    }

    // 扩大时清零旧尾块中新变为有效的范围，防止截断前的旧内容重新出现
    if (needs_tail) {
        uint32_t offset = old_size % NEWFS_BLOCK_SIZE;
        uint32_t count = NEWFS_BLOCK_SIZE - offset;
        if (count > size - old_size)
            count = size - old_size;
        memset(inode->data[tail] + offset, 0, count);
        inode->dirty[tail] = 1;
    }
    inode->size = size;
    return 0;

fail:
    // 只归还本次新增的编号和缓存，保留原来的文件大小、内容及已有缓存
    while (inode->block_count > old_blocks) {
        int block = inode->data_block[--inode->block_count];
        super.data_map[block / 8] &= (uint8_t)~(1u << (block % 8));
        inode->data_block[inode->block_count] = 0;
    }
    for (int block = 0; block < NEWFS_DIRECT_BLOCKS; block++)
        free(new_data[block]);
    if (tail_loaded) {
        free(inode->data[tail]);
        inode->data[tail] = NULL;
    }
    return result;
}

// 根据绝对路径逐级查找 inode，经过的目录按需加载；符号链接由内核配合 readlink 解析
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
        size_t length = strcspn(part, "/"); // 本级文件名的长度
        if (length >= MAX_NAME_LEN)
            return -ENAMETOOLONG;
        int result = load_directory(inode);
        if (result < 0)
            return result;
        struct newfs_dentry *child = inode->children;
        while (child && (strlen(child->name) != length ||
                         memcmp(child->name, part, length) != 0))
            child = child->next;
        if (!child)
            return -ENOENT;
        inode = child->inode; // 进入匹配的子项，继续处理后面的路径
        part += length;
    }
    *out = inode;
    return 0;
}

// 查找 path 的父目录 inode，根目录没有父目录，返回 -EINVAL
static int lookup_parent(const char *path, struct newfs_inode **parent) {
    if (!path || path[0] != '/')
        return -EINVAL;

    // 跳过末尾斜杠，再从后向前找到最后一级名字的起点
    const char *name = path + strlen(path);
    while (name > path + 1 && name[-1] == '/')
        name--;
    if (name == path + 1)
        return -EINVAL;
    while (name[-1] != '/')
        name--;

    char *parent_path = name == path + 1 ? strdup("/") : strndup(path, name - path - 1);
    if (!parent_path)
        return -ENOMEM;
    int result = lookup(parent_path, parent);
    free(parent_path);
    if (result < 0)
        return result;
    if ((*parent)->ftype != NEWFS_DIR)
        return -ENOTDIR;
    // 创建、删除、移动操作都需要访问父目录的子项链表
    return load_directory(*parent);
}

// 创建目录、普通文件或符号链接；created 非 NULL 时返回新 inode，便于继续填充内容
static int create_node(const char *path, FILE_TYPE type, struct newfs_inode **created) {
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
    struct newfs_inode *parent;
    int result = lookup_parent(path, &parent);
    if (result < 0)
        return result;

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
    inode->link = 1;
    inode->ftype = type;
    if (type == NEWFS_DIR) {
        // 新目录已经确定为空，无需从尚未写入的磁盘内容中加载子项
        inode->children_loaded = 1;
        inode->parent = parent;
    }
    memcpy(entry->name, name, length + 1);
    entry->ftype = type;
    entry->inode = inode;
    *tail = entry;
    parent->dir_cnt++;
    parent->size = parent->dir_cnt * sizeof(struct newfs_dentry_d);
    super.inode_cache[ino] = inode;
    // mkdir 和 mknod 不需要新对象，symlink 需要它来保存目标路径
    if (created)
        *created = inode;
    return 0;
}

// 回收已无链接的非目录 inode 或已移除的空目录，同时释放位图编号和内存
static void drop_inode(struct newfs_inode *inode) {
    // 归还数据位图
    for (uint32_t i = 0; i < inode->block_count; i++) {
        int block = inode->data_block[i];
        super.data_map[block / 8] &= (uint8_t)~(1u << (block % 8));
    }
    // 归还 inode 位图
    super.ino_map[inode->ino / 8] &= (uint8_t)~(1u << (inode->ino % 8));
    // 清掉缓存槽位，防止之后访问已经释放的对象，也允许此编号重新分配
    super.inode_cache[inode->ino] = NULL;
    free_inode_memory(inode);
}

// 最后一个硬链接消失时才真正回收资源
static void drop_link(struct newfs_inode *inode) {
    if (--inode->link == 0)
        drop_inode(inode);
}

// entry 指向链表中保存目标目录项指针的位置，只移除目录项，不释放 inode
static void drop_dentry(struct newfs_inode *parent, struct newfs_dentry **entry) {
    struct newfs_dentry *removed = *entry;
    *entry = removed->next;
    free(removed);
    parent->dir_cnt--;
    parent->size = parent->dir_cnt * sizeof(struct newfs_dentry_d);

    // 写回时链表会重新紧密排列，因此无论删掉哪一项，都只需回收末尾多余的块
    uint32_t needed_blocks = (parent->dir_cnt + NEWFS_DENTRIES_PER_BLOCK - 1)
                             / NEWFS_DENTRIES_PER_BLOCK;
    while (parent->block_count > needed_blocks) {
        int block = parent->data_block[--parent->block_count];
        super.data_map[block / 8] &= (uint8_t)~(1u << (block % 8));
        parent->data_block[parent->block_count] = 0;
    }
}

// 判断 target 是否位于 root 的目录树中，包含 root 自身
static int contains_inode(const struct newfs_inode *root, const struct newfs_inode *target) {
    for (const struct newfs_inode *parent = target; parent; parent = parent->parent) {
        if (parent == root)
            return 1;
    }
    return 0;
}

// 写回一个 inode 的目录项或脏数据块，再保存其元数据及链接计数
static int sync_inode(const struct newfs_inode *inode) {
    // 只写回已加载的目录，未加载目录的盘上内容保持不变
    if (inode->ftype == NEWFS_DIR && inode->children_loaded) {
        struct newfs_dentry *child = inode->children;
        for (uint32_t block = 0; block < inode->block_count; block++) {
            // 按链表顺序紧密存放盘上目录项，块内未使用的部分补零
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

    // 普通文件和符号链接只写回脏块，未修改的块保留盘上内容
    else if (inode->ftype != NEWFS_DIR) {
        for (uint32_t block = 0; block < inode->block_count; block++) {
            if (!inode->dirty[block])
                continue;

            off_t offset = (off_t)(super.data_offset + inode->data_block[block]) * NEWFS_BLOCK_SIZE;
            int result = write_disk(offset, inode->data[block], NEWFS_BLOCK_SIZE);
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
        .link = inode->link
    };
    memcpy(disk_inode.data_block, inode->data_block, sizeof(disk_inode.data_block));
    return write_inode(&disk_inode);
}

// 按缓存同步，避免硬链接重复写回，也不会漏掉仅被未加载目录引用的 inode
static int sync_tree(void) {
    // 先同步非目录 inode，再同步目录；每个缓存槽位只处理一次
    for (int directory = 0; directory <= 1; directory++) {
        for (int ino = 0; ino < super.ino_max; ino++) {
            struct newfs_inode *inode = super.inode_cache[ino];
            if (!inode || (inode->ftype == NEWFS_DIR) != directory)
                continue;
            int result = sync_inode(inode);
            if (result < 0)
                return result;
        }
    }
    return 0;
}

// 挂载失败或卸载后释放缓存并关闭设备，不回收磁盘上的 inode 或数据块
static void release_mount(void) {
    if (super.inode_cache) {
        // 按编号释放共享 inode，避免多个硬链接导致同一对象被重复释放
        for (int ino = 0; ino < super.ino_max; ino++)
            free_inode_memory(super.inode_cache[ino]);
        free(super.inode_cache);
    }
    if (super.fd >= 0)
        ddriver_close(super.fd);
    // 清除已失效的指针和布局信息，恢复设备未打开的状态
    super = (struct newfs_super) { .fd = -1 };
}

/******************************************************************************
* SECTION: 必做函数实现
*******************************************************************************/
/**
 * @brief 挂载文件系统，初始化空盘或加载根 inode，目录子项按需读取。
 *
 * @param conn_info FUSE 连接信息
 * @return void* 成功时返回内存超级块地址
 */
void *newfs_init(struct fuse_conn_info *conn_info) {
    int result;
    struct newfs_super_d disk_super;

    // 静态位图在挂载期间一直有效，由超级块保存地址，卸载时无需 free
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
    int first_mount = disk_super.magic_number != NEWFS_MAGIC;
    if (first_mount) {
        super.inode_offset = 3;
        super.inode_blks = NEWFS_INODE_BLOCKS;
        super.data_offset = 3 + NEWFS_INODE_BLOCKS;
        super.data_blks = super.blks_nums - super.data_offset;
        super.ino_max = NEWFS_INODE_LIMIT;
        super.file_max = NEWFS_DIRECT_BLOCKS * NEWFS_BLOCK_SIZE;

        memset(ino_map, 0, sizeof(ino_map));
        memset(data_map, 0, sizeof(data_map));

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
    }
    if (super.ino_max <= 0 || super.ino_max > NEWFS_INODE_LIMIT) {
        result = -EINVAL;
        goto fail;
    }
    // 这里只分配指针数组，具体 inode 在创建或首次访问时才申请内存
    super.inode_cache = calloc(super.ino_max, sizeof(*super.inode_cache));
    if (!super.inode_cache) {
        result = -ENOMEM;
        goto fail;
    }
    if (first_mount) {
        super.root_inode = calloc(1, sizeof(*super.root_inode));
        if (!super.root_inode) {
            result = -ENOMEM;
            goto fail;
        }
        // 保留编号 0 给根目录，空目录已经加载，根 inode 也纳入统一缓存
        ino_map[0] = 1;
        super.root_inode->ino = 0;
        super.root_inode->link = 2; // 根目录按实验约定报告链接数 2
        super.root_inode->ftype = NEWFS_DIR;
        super.root_inode->children_loaded = 1;
        super.inode_cache[0] = super.root_inode;
    } else {
        // 重挂只加载根 inode，访问某个目录时再读取它的子项
        result = load_inode(disk_super.root_ino, &super.root_inode);
        if (result < 0)
            goto fail;
    }

    return &super;

fail:
    fprintf(stderr, "newfs: mount failed: %s\n", strerror(-result));
    release_mount();
    // init 返回 NULL 本身不能通知 FUSE 停止挂载，需要主动请求退出
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
        // 从当前内存布局重新组装盘上超级块，不长期保留另一份全局副本
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
        // 按 inode 缓存写回目录项、修改过的文件内容和 inode 元数据
        if (result == 0) 
            result = sync_tree();
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
    int result = create_node(path, NEWFS_DIR, NULL);
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
    int result = create_node(path, NEWFS_FILE, NULL);
    return result;
}

/**
 * @brief 查找文件、目录或符号链接，读取 inode 并填充文件属性。
 * @param path 相对于文件系统根目录的绝对路径
 * @param stat 接收文件类型、大小、权限等属性的结构体
 * @return int 成功返回 0，失败返回负错误码
 */
int newfs_getattr(const char *path, struct stat *stat) {
    struct newfs_inode *inode;
    int result = lookup(path, &inode);
    if (result == 0) {
        memset(stat, 0, sizeof(*stat));
        stat->st_ino = inode->ino;
        stat->st_mode = (inode->ftype == NEWFS_DIR ? S_IFDIR :
                         inode->ftype == NEWFS_SYMLINK ? S_IFLNK : S_IFREG) | NEWFS_DEFAULT_PERM;
        stat->st_nlink = inode->link; // 不同硬链接报告同一个 inode 的实际链接计数
        stat->st_uid = getuid();
        stat->st_gid = getgid();
        stat->st_size = inode->size;
        stat->st_blksize = NEWFS_BLOCK_SIZE; // 建议的读写块大小，本文件系统为 1024 B
        // st_blocks 使用 512 B 为单位，本实验驱动的 sz_io 正好是 512 B
        stat->st_blocks = inode->block_count * (NEWFS_BLOCK_SIZE / super.sz_io);
        // 当前未在 inode 中维护时间戳，查询时统一填写当前时间
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
    if (result == 0)
        result = load_directory(inode);
    if (result == 0) {
        off_t index = 0;
        for (struct newfs_dentry *child = inode->children; child; child = child->next, index++) {
            // index + 1 告诉 FUSE 下次从哪里继续，filler 非零表示缓冲区已满
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
 * @brief 按块缓存并写入普通文件，按需分配数据块，修改内容在卸载时写回。
 * 
 * @param path 相对于挂载点的路径
 * @param buf 写入的内容
 * @param size 写入的字节数
 * @param offset 相对文件的偏移
 * @param fi 可忽略
 * @return int 成功返回写入字节数，失败返回负错误码
 */
int newfs_write(const char* path, const char* buf, size_t size, off_t offset,
		        struct fuse_file_info* fi) {
    (void)fi;
    struct newfs_inode *inode;
    int result = lookup(path, &inode);
    if (result < 0)
        return result;
    if (inode->ftype == NEWFS_DIR)
        return -EISDIR;
    if (inode->ftype == NEWFS_SYMLINK)
        return -ELOOP;
    if (offset < 0)
        return -EINVAL;
    if (size == 0)
        return 0;
    if (offset > super.file_max || size > (size_t)(super.file_max - offset))
        return -EFBIG;

    uint32_t end = (uint32_t)offset + size; // 写入结束后的字节位置，不包含该位置
    uint32_t first = offset / NEWFS_BLOCK_SIZE;
    uint32_t last = (end - 1) / NEWFS_BLOCK_SIZE;
    uint8_t loaded[NEWFS_DIRECT_BLOCKS] = {0};
    // 先准备涉及的缓存块，失败时撤销本次缓存分配，原内容和大小不变
    for (uint32_t block = first; block <= last; block++) {
        if (!inode->data[block]) {
            result = load_file_block(inode, block);
            if (result < 0)
                goto fail;
            loaded[block] = 1;
        }
    }
    // 扩大文件并为跨过旧文件末尾的空隙补零，之后再覆盖实际写入的范围
    if (end > inode->size) {
        result = resize_file(inode, end);
        if (result < 0)
            goto fail;
    }

    // 一次复制不越过当前逻辑块末尾，跨块写入时继续处理下一块
    size_t done = 0;
    while (done < size) {
        uint32_t block = offset / NEWFS_BLOCK_SIZE;
        size_t offset_in_block = offset % NEWFS_BLOCK_SIZE;
        size_t count = NEWFS_BLOCK_SIZE - offset_in_block;
        if (count > size - done)
            count = size - done;
        memcpy(inode->data[block] + offset_in_block, buf + done, count);
        inode->dirty[block] = 1;
        offset += count;
        done += count;
    }
    return (int)size;

fail:
    // 只释放本次预加载的缓存，resize_file 自行撤销它新增的磁盘块和内存
    for (uint32_t block = first; block <= last; block++) {
        if (loaded[block]) {
            free(inode->data[block]);
            inode->data[block] = NULL;
        }
    }
    return result;
}

/**
 * @brief 按需加载并读取普通文件的数据块，读取到文件末尾时返回 0。
 * 
 * @param path 相对于挂载点的路径
 * @param buf 读取的内容
 * @param size 读取的字节数
 * @param offset 相对文件的偏移
 * @param fi 可忽略
 * @return int 成功返回实际读取字节数，失败返回负错误码
 */
int newfs_read(const char* path, char* buf, size_t size, off_t offset,
		       struct fuse_file_info* fi) {
    (void)fi;
    struct newfs_inode *inode;
    int result = lookup(path, &inode);
    if (result < 0)
        return result;
    if (inode->ftype == NEWFS_DIR)
        return -EISDIR;
    if (inode->ftype == NEWFS_SYMLINK)
        return -ELOOP;
    if (offset < 0)
        return -EINVAL;
    if (size == 0 || offset >= inode->size)
        return 0;

    // 读取长度不能超过文件末尾，避免返回尾块中的无效字节
    size_t remaining = inode->size - offset;
    if (size > remaining)
        size = remaining;
    size_t done = 0;
    while (done < size) {
        uint32_t block = offset / NEWFS_BLOCK_SIZE;
        result = load_file_block(inode, block);
        if (result < 0)
            return done > 0 ? (int)done : result;
        size_t offset_in_block = offset % NEWFS_BLOCK_SIZE;
        size_t count = NEWFS_BLOCK_SIZE - offset_in_block;
        if (count > size - done)
            count = size - done;
        memcpy(buf + done, inode->data[block] + offset_in_block, count);
        offset += count;
        done += count;
    }
    return (int)done;
}

/**
 * @brief 创建硬链接，只增加目录项和链接计数，不复制 inode 或文件内容。
 * @param from 已存在的非目录路径
 * @param to 新链接的绝对路径
 * @return int 成功返回 0，失败返回负错误码
 */
int newfs_link(const char *from, const char *to) {
    struct newfs_inode *inode;
    int result = lookup(from, &inode);
    if (result < 0)
        return result;
    // 目录不允许创建硬链接，避免出现多个父目录或目录循环
    if (inode->ftype == NEWFS_DIR)
        return -EPERM;
    if (inode->link == UINT32_MAX)
        return -EMLINK;
    if (!to || to[0] != '/')
        return -EINVAL;
    // 硬链接的新名字必须合法，目标路径是否已存在由父目录链表判断
    const char *name = strrchr(to, '/') + 1;
    size_t length = strlen(name);
    if (!length)
        return -EEXIST;
    if (length >= MAX_NAME_LEN)
        return -ENAMETOOLONG;
    if (!strcmp(name, ".") || !strcmp(name, ".."))
        return -EINVAL;

    struct newfs_inode *parent;
    result = lookup_parent(to, &parent);
    if (result < 0)
        return result;
    struct newfs_dentry **tail = &parent->children;
    while (*tail) {
        if (!strcmp((*tail)->name, name))
            return -EEXIST;
        tail = &(*tail)->next;
    }
    if (parent->dir_cnt >= NEWFS_DIRECT_BLOCKS * NEWFS_DENTRIES_PER_BLOCK)
        return -ENOSPC;
    // 文件本身不分配新 inode，但新目录项仍可能需要扩充父目录的数据块
    struct newfs_dentry *entry = calloc(1, sizeof(*entry));
    if (!entry)
        return -ENOMEM;
    if (parent->dir_cnt % NEWFS_DENTRIES_PER_BLOCK == 0) {
        int block = alloc_bitmap(super.data_map, super.data_blks);
        if (block < 0) {
            free(entry);
            return block;
        }
        parent->data_block[parent->block_count++] = block;
    }
    memcpy(entry->name, name, length + 1);
    entry->ftype = inode->ftype;
    entry->inode = inode;
    *tail = entry;
    parent->dir_cnt++;
    parent->size = parent->dir_cnt * sizeof(struct newfs_dentry_d);
    // 新名字直接引用同一个缓存 inode，任何一个硬链接的读写都会作用于它
    inode->link++;
    return 0;
}

/**
 * @brief 创建符号链接，将目标路径原样保存到独立 inode 的数据块中。
 * @param target 目标路径，可以是相对路径或尚不存在的路径
 * @param linkpath 新链接的绝对路径
 * @return int 成功返回 0，失败返回负错误码
 */
int newfs_symlink(const char *target, const char *linkpath) {
    if (!target)
        return -EINVAL;
    size_t size = strlen(target);
    if (!size)
        return -ENOENT;
    if (size > (size_t)super.file_max)
        return -ENAMETOOLONG;
    // 先保留父目录，后续内容分配失败时用于撤销新建的目录项
    struct newfs_inode *parent;
    int result = lookup_parent(linkpath, &parent);
    if (result < 0)
        return result;
    struct newfs_inode *inode;
    // 符号链接使用独立 inode，created 输出新对象，便于继续写入目标路径
    result = create_node(linkpath, NEWFS_SYMLINK, &inode);
    if (result < 0)
        return result;
    // 复用文件扩容逻辑，按路径长度分配数据块和全零缓存，并标记为脏块
    result = resize_file(inode, (uint32_t)size);
    if (result < 0) {
        // 父目录已经加载，直接撤销刚创建的目录项
        struct newfs_dentry **entry = &parent->children;
        while ((*entry)->inode != inode)
            entry = &(*entry)->next;
        drop_dentry(parent, entry);
        drop_link(inode);
        return result;
    }
    // 只保存路径原文，不检查目标是否存在，也不把相对路径转成绝对路径
    // size 不包含字符串结束符，结束符由 readlink 返回时补上
    for (uint32_t block = 0; block < inode->block_count; block++) {
        size_t start = block * NEWFS_BLOCK_SIZE;
        size_t count = size - start;
        if (count > NEWFS_BLOCK_SIZE)
            count = NEWFS_BLOCK_SIZE;
        memcpy(inode->data[block], target + start, count);
    }
    return 0;
}

/**
 * @brief 读取符号链接保存的目标路径，最多复制 size-1 字节并补字符串结束符。
 * @param path 链接自身的绝对路径
 * @param buf 目标路径缓冲区
 * @param size 缓冲区大小，包含字符串结束符
 * @return int 成功返回 0，失败返回负错误码
 */
int newfs_readlink(const char *path, char *buf, size_t size) {
    if (!buf || size == 0)
        return -EINVAL;
    struct newfs_inode *inode;
    int result = lookup(path, &inode);
    if (result < 0)
        return result;
    if (inode->ftype != NEWFS_SYMLINK)
        return -EINVAL;
    // 预留一个字节给结束符，缓冲区不足时只返回能容纳的前缀
    size_t count = inode->size;
    if (count >= size)
        count = size - 1;
    size_t done = 0;
    // 目标路径也可能跨多个块，只有读取到的块才加载到缓存
    while (done < count) {
        uint32_t block = done / NEWFS_BLOCK_SIZE;
        result = load_file_block(inode, block);
        if (result < 0)
            return result;
        size_t bytes = count - done;
        if (bytes > NEWFS_BLOCK_SIZE)
            bytes = NEWFS_BLOCK_SIZE;
        memcpy(buf + done, inode->data[block], bytes);
        done += bytes;
    }
    buf[count] = '\0';
    // FUSE 的 readlink 回调成功返回 0，路径通过 buf 交给内核继续解析
    return 0;
}

/**
 * @brief 删除文件名或符号链接，最后一个硬链接消失时回收 inode 和数据块。
 * @param path 相对于文件系统根目录的绝对路径
 * @return int 成功返回 0，失败返回负错误码
 */
int newfs_unlink(const char* path) {
    struct newfs_inode *inode;
    int result = lookup(path, &inode);
    if (result < 0)
        return result;
    if (inode->ftype == NEWFS_DIR)
        return -EISDIR;

    // 找到父目录，以及保存该文件名的目录项
    struct newfs_inode *parent;
    result = lookup_parent(path, &parent);
    if (result < 0)
        return result;

    // 同一目录中可能有多个名字指向此 inode，必须按名字找到要删的目录项
    const char *name = strrchr(path, '/') + 1;
    struct newfs_dentry **entry = &parent->children;
    while (*entry && strcmp((*entry)->name, name))
        entry = &(*entry)->next;
    if (!*entry)
        return -ENOENT;

    // 先删除这个名字，再减少链接数；其他硬链接仍存在时保留 inode 和内容
    drop_dentry(parent, entry);
    drop_link(inode);
    return 0;
}

/**
 * @brief 删除空目录，非空目录返回 -ENOTEMPTY，根目录不可删除。
 * @param path 相对于文件系统根目录的绝对路径
 * @return int 成功返回 0，失败返回负错误码
 */
int newfs_rmdir(const char* path) {
    struct newfs_inode *inode;
    int result = lookup(path, &inode);
    if (result < 0)
        return result;
    if (inode->ftype != NEWFS_DIR)
        return -ENOTDIR;
    if (inode == super.root_inode)
        return -EBUSY;
    // 未加载目录的 children 可能为 NULL，必须同时根据盘上恢复的 dir_cnt 判断
    if (inode->children || inode->dir_cnt != 0)
        return -ENOTEMPTY;

    struct newfs_inode *parent;
    result = lookup_parent(path, &parent);
    if (result < 0)
        return result;

    struct newfs_dentry **entry = &parent->children;
    while (*entry && (*entry)->inode != inode)
        entry = &(*entry)->next;
    if (!*entry)
        return -ENOENT;

    drop_dentry(parent, entry);
    drop_inode(inode);
    return 0;
}

/**
 * @brief 重命名或移动目录项，保留源 inode，可覆盖非目录文件或空目录，目录与非目录不能互换。
 * @param from 源文件或目录的绝对路径
 * @param to 目标绝对路径，包含新名字
 * @return int 成功返回 0，失败返回负错误码
 */
int newfs_rename(const char* from, const char* to) {
    struct newfs_inode *inode;
    int result = lookup(from, &inode);
    if (result < 0)
        return result;
    if (inode == super.root_inode)
        return -EBUSY;
    if (!to || to[0] != '/')
        return -EINVAL;

    // 提取新旧文件名，目录路径允许带末尾斜杠
    const char *from_end = from + strlen(from);
    while (from_end[-1] == '/')
        from_end--;
    const char *from_name = from_end;
    while (from_name[-1] != '/')
        from_name--;
    size_t from_length = from_end - from_name;

    const char *to_end = to + strlen(to);
    while (to_end > to + 1 && to_end[-1] == '/')
        to_end--;
    if (to_end == to + 1)
        return -EBUSY;
    if (*to_end == '/' && inode->ftype != NEWFS_DIR)
        return -ENOTDIR;
    const char *to_name = to_end;
    while (to_name[-1] != '/')
        to_name--;
    size_t to_length = to_end - to_name;
    if (to_length >= MAX_NAME_LEN)
        return -ENAMETOOLONG;
    if ((to_length == 1 && to_name[0] == '.') ||
        (to_length == 2 && to_name[0] == '.' && to_name[1] == '.'))
        return -EINVAL;

    // 获取新旧文件的父 inode
    struct newfs_inode *from_parent, *to_parent;
    result = lookup_parent(from, &from_parent);
    if (result < 0)
        return result;
    result = lookup_parent(to, &to_parent);
    if (result < 0)
        return result;
    // 不能将目录移入自身或它的子目录，否则 parent 链和盘上目录结构会出现循环
    if (inode->ftype == NEWFS_DIR && contains_inode(inode, to_parent))
        return -EINVAL;

    // 获取新旧文件的 entry
    struct newfs_dentry **from_entry = &from_parent->children;
    while (*from_entry && (strlen((*from_entry)->name) != from_length ||
                           memcmp((*from_entry)->name, from_name, from_length)))
        from_entry = &(*from_entry)->next;
    if (!*from_entry)
        return -ENOENT;
    struct newfs_dentry *source = *from_entry;

    struct newfs_dentry **to_entry = &to_parent->children;
    while (*to_entry && (strlen((*to_entry)->name) != to_length ||
                         memcmp((*to_entry)->name, to_name, to_length)))
        to_entry = &(*to_entry)->next;
    struct newfs_dentry *target = *to_entry;
    // 判断是否允许覆盖
    if (target) {
        // 两个名字已经引用同一 inode 时不做修改，也不减少硬链接计数
        if (target->inode == inode)
            return 0;
        if (inode->ftype == NEWFS_DIR && target->ftype != NEWFS_DIR)
            return -ENOTDIR;
        if (inode->ftype != NEWFS_DIR && target->ftype == NEWFS_DIR)
            return -EISDIR;
        if (target->ftype == NEWFS_DIR &&
            (target->inode->children || target->inode->dir_cnt != 0))
            return -ENOTEMPTY;
    }

    // 同一父目录中直接改名，无需为源文件再分配目录项或 inode
    if (from_parent == to_parent) {
        if (target) {
            struct newfs_inode *old_inode = target->inode;
            drop_dentry(to_parent, to_entry);
            drop_link(old_inode);
        }
        memcpy(source->name, to_name, to_length);
        source->name[to_length] = '\0';
        return 0;
    }

    if (target) {
        // 跨目录覆盖时复用目标目录项，旧目标仅减少链接数，归零后才回收
        struct newfs_inode *old_inode = target->inode;
        target->inode = inode;
        target->ftype = source->ftype; // 普通文件和符号链接可互相覆盖，目录项类型也要更新
        drop_dentry(from_parent, from_entry);
        drop_link(old_inode);
    } else {
        if (to_parent->dir_cnt >= NEWFS_DIRECT_BLOCKS * NEWFS_DENTRIES_PER_BLOCK)
            return -ENOSPC;
        struct newfs_dentry *entry = calloc(1, sizeof(*entry));
        if (!entry)
            return -ENOMEM;
        memcpy(entry->name, to_name, to_length);
        entry->ftype = source->ftype;
        entry->inode = inode;

        // 所有可能失败的分配先完成，失败时保留原路径及其内容
        if (to_parent->dir_cnt % NEWFS_DENTRIES_PER_BLOCK == 0) {
            int block = alloc_bitmap(super.data_map, super.data_blks);
            if (block < 0) {
                free(entry);
                return block;
            }
            to_parent->data_block[to_parent->block_count++] = block;
        }
        // 先把新目录项接入目标父目录，再移除源目录项，源 inode 链接数不变
        *to_entry = entry;
        to_parent->dir_cnt++;
        to_parent->size = to_parent->dir_cnt * sizeof(struct newfs_dentry_d);
        drop_dentry(from_parent, from_entry);
    }
    // 目录移动后更新父指针，供后续路径加载和目录循环检查使用
    if (inode->ftype == NEWFS_DIR)
        inode->parent = to_parent;
    return 0;
}

/**
 * @brief 打开普通文件，检查路径存在及类型，内容仍在读写时按需加载。
 * @param path 相对于文件系统根目录的绝对路径
 * @param fi 文件信息，当前忽略
 * @return int 成功返回 0，失败返回负错误码
 */
int newfs_open(const char* path, struct fuse_file_info* fi) {
    (void)fi;
    struct newfs_inode *inode;
    int result = lookup(path, &inode);
    if (result < 0)
        return result;
    if (inode->ftype == NEWFS_DIR)
        return -EISDIR;
    // 正常打开符号链接时，内核先通过 readlink 解析，再对目标执行 open
    if (inode->ftype == NEWFS_SYMLINK)
        return -ELOOP;
    return 0;
}

/**
 * @brief 打开目录并检查类型，子项仍在查找或列举时按需加载。
 * @param path 相对于文件系统根目录的绝对路径
 * @param fi 文件信息，当前忽略
 * @return int 成功返回 0，失败返回负错误码
 */
int newfs_opendir(const char* path, struct fuse_file_info* fi) {
    (void)fi;
    struct newfs_inode *inode;
    int result = lookup(path, &inode);
    if (result < 0)
        return result;
    if (inode->ftype != NEWFS_DIR)
        return -ENOTDIR;
    return 0;
}

/**
 * @brief 调整普通文件大小，扩大时补零，缩小时回收多余数据块。
 * @param path 相对于文件系统根目录的绝对路径
 * @param offset 改变后的文件大小，单位为字节
 * @return int 成功返回 0，失败返回负错误码
 */
int newfs_truncate(const char* path, off_t offset) {
    struct newfs_inode *inode;
    int result = lookup(path, &inode);
    if (result < 0)
        return result;
    if (inode->ftype == NEWFS_DIR)
        return -EISDIR;
    if (inode->ftype == NEWFS_SYMLINK)
        return -ELOOP;
    if (offset < 0)
        return -EINVAL;
    if (offset > super.file_max)
        return -EFBIG;
    return resize_file(inode, (uint32_t)offset);
}


/**
 * @brief 检查路径是否存在及请求的访问权限，使用 getattr 报告的默认权限。
 * 
 * @param path 相对于文件系统根目录的绝对路径
 * @param type 访问类别
 * R_OK: Test for read permission. 
 * W_OK: Test for write permission.
 * X_OK: Test for execute permission.
 * F_OK: Test for existence. 
 * 
 * @return int 成功返回 0，失败返回负错误码
 */
int newfs_access(const char* path, int type) {
    // F_OK 为 0，仅要求路径存在；其他合法位分别表示读、写、执行权限
    if (type & ~(R_OK | W_OK | X_OK))
        return -EINVAL;
    struct stat st;
    int result = newfs_getattr(path, &st);
    if (result < 0)
        return result;
    if (((type & R_OK) && !(st.st_mode & 0444)) ||
        ((type & W_OK) && !(st.st_mode & 0222)) ||
        ((type & X_OK) && !(st.st_mode & 0111)))
        return -EACCES;
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
