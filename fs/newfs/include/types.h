#ifndef _TYPES_H_
#define _TYPES_H_

#include <stdint.h>

#define MAX_NAME_LEN    128

struct custom_options {
	const char* device;
};

typedef enum {
    NEWFS_FILE,   // 普通文件
    NEWFS_DIR     // 目录文件
} FILE_TYPE;

/* to-Disk */
struct newfs_super_d {
    uint32_t magic_number; // 幻数，用来识别磁盘上是否已经存在目标文件系统

    int ino_map_offset;    // inode 位图区的起始逻辑块号
    int ino_map_blks;      // inode 位图区域占多少个逻辑块

    int data_map_offset;  // 数据块位图区的起始逻辑块号
    int data_map_blks;

    int inode_offset;     // inode 区的起始逻辑块号
    int inode_blks;

    int data_offset;      // 数据区的起始逻辑块号
    int data_blks;

    int ino_max;           // 最多支持多少个 inode
    int file_max;          // 单个文件允许的最大大小

    uint32_t root_ino;          // 根目录对应的 inode 编号
};

struct newfs_inode_d {
    uint32_t ino;          // inode 编号，用来标识这是第几个 inode

    FILE_TYPE ftype;       // 文件类型，例如普通文件或目录
    int dir_cnt;           // 如果是目录，可记录当前目录下有多少个子项

    uint32_t size;              // 文件当前大小
    uint32_t block_count;
    int data_block[6];     // 数据区内的相对块号，前 block_count 个槽位有效

    uint32_t link;         // 链接数；本实验必做部分不要求专门处理，通常可默认为 1
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
    int file_max;                    // 单个普通文件内容的最大字节数

    uint8_t *ino_map;                // 指向内存中的 inode 位图
    uint8_t *data_map;               // 指向内存中的数据块位图
    struct newfs_inode *root_inode;  // 指向已读入内存的根 inode，供路径查找使用
};

struct newfs_inode {
    uint32_t ino;
    FILE_TYPE ftype;
    int dir_cnt;

    uint32_t size;
    uint32_t block_count;
    int data_block[6];
    char *data[6];                    // 普通文件的逐块缓存，NULL 表示该块尚未加载
    uint8_t dirty[6];                 // 对应缓存块是否被修改，需要写回

    struct newfs_dentry *children;
};

struct newfs_dentry {
    char name[MAX_NAME_LEN];
    FILE_TYPE ftype;

    struct newfs_inode *inode;
    struct newfs_dentry *next;
};

#endif /* _TYPES_H_ */
