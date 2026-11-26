#ifndef _TYPES_H_
#define _TYPES_H_

#include <stdint.h>

#define MAX_NAME_LEN    128

struct custom_options {
	const char* device;
};

typedef enum {
    NEWFS_FILE,   // 普通文件
    NEWFS_DIR,    // 目录文件
    NEWFS_SYMLINK // 符号链接，数据块中保存目标路径
} FILE_TYPE;

/* to-Disk */
struct newfs_super_d {
    uint32_t magic_number; // 幻数，用来识别磁盘上是否已经存在目标文件系统

    int ino_map_offset;    // inode 位图区的起始逻辑块号
    int ino_map_blks;      // inode 位图区域占多少个逻辑块

    int data_map_offset;  // 数据块位图区的起始逻辑块号
    int data_map_blks;    // 数据块位图区占用的逻辑块数

    int inode_offset;     // inode 区的起始逻辑块号
    int inode_blks;       // inode 区占用的逻辑块数

    int data_offset;      // 数据区的起始逻辑块号
    int data_blks;        // 数据区的逻辑块总数

    int ino_max;           // 最多支持多少个 inode
    int file_max;          // 单个文件允许的最大字节数，符号链接的目标路径也受此限制

    uint32_t root_ino;          // 根目录对应的 inode 编号
};

struct newfs_inode_d {
    uint32_t ino;          // inode 编号，用来标识这是第几个 inode

    FILE_TYPE ftype;       // 文件类型：普通文件、目录或符号链接
    int dir_cnt;           // 如果是目录，可记录当前目录下有多少个子项

    uint32_t size;              // 文件内容的字节数，目录则为所有盘上目录项的总大小
    uint32_t block_count;       // 实际分配的数据块数量
    int data_block[6];     // 数据区内的相对块号，前 block_count 个槽位有效

    uint32_t link;         // 硬链接计数，最后一个名字删除后才能回收 inode
};

struct newfs_dentry_d {
    uint32_t ino;                    // 该目录项对应的 inode 编号
    char file_name[MAX_NAME_LEN];    // 文件名
    FILE_TYPE ftype;                 // 文件类型
};

/* in-Mem */
struct newfs_super {
    int fd;                          // ddriver_open 返回的设备文件描述符

    int sz_disk;                     // 设备总容量
    int sz_io;                       // 驱动单次读写的大小
    int blks_nums;                   // 逻辑块总数

    int inode_offset;                // inode 区的起始逻辑块号
    int inode_blks;                  // inode 区占用的逻辑块数

    int data_offset;                 // 数据区的起始逻辑块号
    int data_blks;                   // 数据区的逻辑块总数

    int ino_max;                     // 最多可分配的 inode 数量，包含根 inode
    int file_max;                    // 普通文件内容或符号链接目标路径的最大字节数

    uint8_t *ino_map;                // 指向内存中的 inode 位图
    uint8_t *data_map;               // 指向内存中的数据块位图
    struct newfs_inode *root_inode;  // 指向已读入内存的根 inode，供路径查找使用
    struct newfs_inode **inode_cache;// 按 inode 编号索引的指针数组，硬链接共享同一个内存 inode
};

struct newfs_inode {
    uint32_t ino;                     // inode 编号，用于缓存索引和定位盘上记录
    uint32_t link;                    // 硬链接计数，与盘上 inode 保持一致
    FILE_TYPE ftype;                  // 文件类型，与引用它的目录项保持一致
    int dir_cnt;                     // 目录的子项数，普通文件和符号链接为 0

    uint32_t size;                    // 当前内容的字节数，目录为子项数乘以盘上目录项大小
    uint32_t block_count;             // 已分配的数据块数，前 block_count 个块号有效
    int data_block[6];               // 数据区内的相对块号，转换为磁盘块号时需加 data_offset
    char *data[6];                    // 普通文件或符号链接的逐块缓存，NULL 表示该块尚未加载
    uint8_t dirty[6];                 // 对应缓存块是否被修改，需要写回

    struct newfs_dentry *children;    // 本目录子项链表的头指针，非目录不使用
    uint8_t children_loaded;          // 目录的子项是否已加载，区别于已加载的空目录
    struct newfs_inode *parent;       // 目录的父 inode，根目录为 NULL，用于移动时检查循环
};

struct newfs_dentry {
    char name[MAX_NAME_LEN];         // 此目录项的文件名，不包含父目录路径
    FILE_TYPE ftype;                 // 此目录项指向的文件类型

    struct newfs_inode *inode;       // 指向缓存中的 inode，多个硬链接可指向同一对象
    struct newfs_dentry *next;       // 同一父目录下的下一个目录项
};

#endif /* _TYPES_H_ */
