#ifndef SQUASH_TYPES_H
#define SQUASH_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#define SQUASH_API __declspec(dllexport)
#define squash_le16toh(x) (x)
#define squash_le32toh(x) (x)
#define squash_le64toh(x) (x)
#else
#define SQUASH_API
#include <endian.h>
#define squash_le16toh(x) le16toh(x)
#define squash_le32toh(x) le32toh(x)
#define squash_le64toh(x) le64toh(x)
#endif
#define SQUASHFS_METADATA_SIZE 8192
#define SQUASHFS_COMPRESSED_BIT_BLOCK 0x8000
#define SQUASHFS_COMPRESSED_SIZE_MASK 0x7FFF
#define SQUASHFS_COMPRESSED(B) (!((B) & SQUASHFS_COMPRESSED_BIT))

#define SQUASHFS_COMPRESSED_BIT (1 << 15)

#define SQUASHFS_COMPRESSED_SIZE(B) (((B) & ~SQUASHFS_COMPRESSED_BIT) ? (B) & ~SQUASHFS_COMPRESSED_BIT : SQUASHFS_COMPRESSED_BIT)

#define SQUASHFS_DIR_INODE_SIZE    28
#define SQUASHFS_LDIR_INODE_SIZE   32

#define GET_LE16(p) ((uint16_t)(p)[0] | ((uint16_t)(p)[1] << 8))
#define GET_LE32(p) ((uint32_t)(p)[0] | ((uint32_t)(p)[1] << 8) | ((uint32_t)(p)[2] << 16) | ((uint32_t)(p)[3] << 24))

// Типы сжатия
typedef enum
{
    SQUASH_COMPRESSION_GZIP = 1,
    SQUASH_COMPRESSION_LZMA = 2,
    SQUASH_COMPRESSION_LZO = 3,
    SQUASH_COMPRESSION_XZ = 4,
    SQUASH_COMPRESSION_LZ4 = 5,
    SQUASH_COMPRESSION_ZSTD = 6
} squash_compression_t;

// Структура декомпрессора
typedef struct squash_decompressor squash_decompressor_t;

// Основные типы данных SquashFS
typedef uint64_t squash_off_t;
typedef uint32_t squash_size_t;
typedef uint16_t squash_mode_t;
typedef uint32_t squash_uid_t;
typedef uint32_t squash_gid_t;
typedef uint32_t squash_time_t;

// Супер блок SquashFS
typedef struct
{
    uint32_t s_magic;
    uint32_t inodes;
    uint32_t mkfs_time;
    uint32_t block_size;
    uint32_t fragments;
    uint16_t compression;
    uint16_t block_log;
    uint16_t flags;
    uint16_t no_ids;
    uint16_t s_major;
    uint16_t s_minor;
    uint64_t root_inode;
    uint64_t bytes_used;
    uint64_t id_table_start;
    uint64_t xattr_id_table_start;
    uint64_t inode_table_start;
    uint64_t directory_table_start;
    uint64_t fragment_table_start;
    uint64_t lookup_table_start;
} squash_super_t;

// Типы инодов
typedef enum
{
    SQUASHFS_DIR_TYPE = 1,
    SQUASHFS_REG_TYPE = 2,
    SQUASHFS_SYMLINK_TYPE = 3,
    SQUASHFS_BLKDEV_TYPE = 4,
    SQUASHFS_CHRDEV_TYPE = 5,
    SQUASHFS_FIFO_TYPE = 6,
    SQUASHFS_SOCKET_TYPE = 7,
    SQUASHFS_LDIR_TYPE = 8,
    SQUASHFS_LREG_TYPE = 9,
    SQUASHFS_LSYMLINK_TYPE = 10,
    SQUASHFS_LBLKDEV_TYPE = 11,
    SQUASHFS_LCHRDEV_TYPE = 12,
    SQUASHFS_LFIFO_TYPE = 13,
    SQUASHFS_LSOCKET_TYPE = 14
} squash_inode_type_t;

// Базовая структура инода
typedef struct {
    uint16_t inode_type;    // 2
    uint16_t mode;         // 2
    uint16_t uid;          // 2
    uint16_t gid;         // 2
    uint32_t mtime;        // 4
    uint32_t inode_number; // 4
} squash_base_inode_t; // 16 байт

// Структура файла
typedef struct {
    squash_base_inode_t base; // 16
    uint32_t start_block;     // 4
    uint32_t fragment;        // 4
    uint32_t offset;          // 4
    uint32_t file_size;       // 4
    uint32_t *block_list;     // 8 (указатель, не на диске)
} squash_reg_inode_t; // 40 байт в памяти, 16 байт на диске

// Структура директории
typedef struct
{
    uint32_t index;       // 4 байта
    uint32_t start_block; // 4 байта
    uint32_t size;        // 4 байта
    char *name;           // Указатель
} squash_dir_index_t;

typedef struct {
    squash_base_inode_t base; // 16
    uint32_t start_block;     // 4
    uint32_t nlink;           // 4
    uint16_t file_size;       // 2
    uint16_t offset;          // 2
    uint32_t parent_inode;    // 4
    uint16_t i_count;         // 2 (не используется)
    uint32_t xattr_idx;       // 4 (не используется)
    squash_dir_index_t *index; // 8 (указатель, не на диске)
} squash_dir_inode_t; // 48 байт в памяти, 16 байт на диске

typedef struct {
    squash_base_inode_t base; // 16
    uint32_t start_block;     // 4
    uint32_t nlink;           // 4
    uint32_t file_size;       // 4
    uint16_t offset;          // 2
    uint32_t parent_inode;    // 4
    uint32_t i_count;         // 4
    uint32_t xattr_idx;       // 4
    squash_dir_index_t *index; // 8 (указатель, не на диске)
} squash_ldir_inode_t; // 50 байт в памяти, 26 байт на диске

typedef struct {
    squash_base_inode_t base; // 16
    uint32_t nlink;           // 4
    uint32_t target_size;     // 4
    char *target_path;        // 8 (указатель, не на диске)
} squash_symlink_inode_t; // 32 байт в памяти, 8 байт на диске


typedef struct {
    squash_base_inode_t base;
    uint32_t nlink;
    uint32_t rdev;
} squash_dev_inode_t;

typedef struct {
    squash_base_inode_t base;
    uint32_t nlink;
} squash_ipc_inode_t;

// Запись в директории
typedef struct
{
    uint32_t count;        // Количество записей - 1
    uint32_t start_block;  // Начальный блок
    uint32_t inode_number; // Базовый номер инода
} squash_dir_header_t;

typedef struct squash_dir_entry_t
{
    uint64_t inode_ref;
    uint32_t inode_number;
    uint16_t type;
    size_t size; // Длина имени (включая '\0')
    char *name;  // Имя файла/папки (ASCIIZ)
} squash_dir_entry_t;

typedef struct
{
    squash_dir_entry_t **entries;
    size_t count;
    size_t capacity;
} dir_entries_list_t;

struct squashfs_fragment_entry
{
    uint64_t start_block;
    uint32_t size;
    uint32_t unused;
};

// Основная структура для работы с образом
typedef struct squash_fs
{
    FILE *file;
    squash_super_t super;
    squash_decompressor_t *decompressor;
    struct squashfs_fragment_entry *fragment_table;
    uint64_t *inode_lookup_table;
    uint32_t *id_table;
    char *filename;
} squash_fs_t;

// Структура для итерации по директории
typedef struct
{
    squash_fs_t *fs;
    squash_dir_inode_t *dir_inode;
    uint8_t *uncompressed_data;
    size_t uncompressed_size;
    uint32_t header_inode_number;
    uint32_t header_entry_count;
    uint32_t current_header_offset;
    uint32_t start_block;
    uint32_t current_entry_index;
    bool finished;
} squash_dir_iterator_t;

typedef struct
{
    squash_off_t *inodes;
    size_t count;
    size_t capacity;
} squash_visited_inodes_t;

#endif // SQUASH_TYPES_H