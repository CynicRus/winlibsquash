#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/libsquash/squash.h"

SQUASH_API squash_error_t squash_lookup_path(squash_fs_t *fs, const char *path, squash_off_t *inode_ref)
{
    if (!fs || !fs->file || !path || !inode_ref)
        return SQUASH_ERROR_INVALID_FILE;

    *inode_ref = fs->super.root_inode;

    if (path[0] == '\0' || (path[0] == '/' && path[1] == '\0'))
        return SQUASH_OK;

    // Защита от избыточных / и выделение под компонент пути
    char component[1024]; // Максимальная длина компоненты имени директории
    const char *cur = path;
    squash_error_t err;

    squash_visited_inodes_t visited;
    err = squash_visited_inodes_init(&visited, 16);
    if (err != SQUASH_OK)
        return err;
    err = squash_visited_inodes_add(&visited, *inode_ref);
    if (err != SQUASH_OK)
        goto cleanup_visited;

    while (*cur) {
        // Пропускаем начальные / (двойные и более)
        while (*cur == '/')
            cur++;
        if (!*cur) break;

        // имя компоненты в буфер 
        size_t clen = 0;
        while (cur[clen] && cur[clen] != '/' && clen < sizeof(component)-1)
            clen++;
        if (clen == 0)
            break;
        if (clen >= sizeof(component)-1) {
            err = SQUASH_ERROR_NAME_TOO_LONG;
            goto cleanup_visited;
        }
        memcpy(component, cur, clen);
        component[clen] = '\0';
        cur += clen;

        // Чтение текущего inode
        void *current_inode = NULL;
        err = squash_read_inode(fs, *inode_ref, &current_inode);
        if (err != SQUASH_OK)
            goto cleanup_visited;

        if (!squash_is_directory(current_inode)) {
            squash_free_inode(current_inode);
            err = SQUASH_ERROR_NOT_DIRECTORY;
            goto cleanup_visited;
        }

        squash_dir_inode_t *dir_inode = (squash_dir_inode_t *)current_inode;
        squash_dir_iterator_t *iterator = NULL;
        err = squash_opendir(fs, dir_inode, &iterator);
        if (err != SQUASH_OK) {
            squash_free_inode(current_inode);
            goto cleanup_visited;
        }

        bool found = false;
        squash_dir_entry_t *entry = NULL;

        while ((squash_readdir(iterator, &entry) == SQUASH_OK) && entry) {
            if (strcmp(entry->name, component) == 0) {
                // Проверяем на цикл
                if (squash_visited_inodes_contains(&visited, entry->inode_ref)) {
                    squash_free_dir_entry(entry);
                    err = SQUASH_ERROR_CYCLE_DETECTED;
                    found = false;
                    break;
                }
                *inode_ref = entry->inode_ref;
                err = squash_visited_inodes_add(&visited, *inode_ref);
                squash_free_dir_entry(entry);
                if (err != SQUASH_OK) {
                    found = false;
                    break;
                }
                found = true;
                break;
            }
            squash_free_dir_entry(entry);
        }
        squash_closedir(iterator);
        squash_free_inode(current_inode);
        if (!found) {
            err = (err == SQUASH_OK) ? SQUASH_ERROR_NOT_FOUND : err;
            goto cleanup_visited;
        }
        // Следующая компонента
    }

    err = SQUASH_OK;

cleanup_visited:
    squash_visited_inodes_free(&visited);
    return err;
}

// Вспомогательная функция: извлекает block_offset и offset_in_block из inode_ref
static inline void parse_inode_ref(squash_off_t inode_ref, uint64_t *block_offset, uint32_t *offset_in_block)
{
    *block_offset = inode_ref >> 16;
    *offset_in_block = inode_ref & 0xFFFF;
}

// Вспомогательная функция: читает и распаковывает metadata-блок с inode
static squash_error_t load_inode_metablock(
    squash_fs_t *fs, uint64_t block_offset,
    uint8_t **uncompressed_data, size_t *uncompressed_size)
{
    uint64_t metablock_offset = fs->super.inode_table_start + block_offset;
    size_t compressed_size = 0;
    return squash_read_metadata_block(fs, metablock_offset, uncompressed_data, uncompressed_size, &compressed_size);
}

// Разбор основного типа и базовой части inode
static squash_error_t parse_base_inode(
    const uint8_t *uncompressed_data, size_t uncompressed_size,
    uint32_t *offset_in_block, squash_base_inode_t *base, uint16_t *inode_type)
{
   // printf("parse_base_inode(): offset_in_block=%u, uncompressed_size=%zu\n", *offset_in_block, uncompressed_size);
    
    if (*offset_in_block + sizeof(uint16_t) > uncompressed_size) {
        fprintf(stderr, "ERROR: offset_in_block=%u > uncompressed_size=%zu (need +2)\n", *offset_in_block, uncompressed_size);
        return SQUASH_ERROR_INVALID_INODE;
    }

    *inode_type = *(uint16_t *)(uncompressed_data + *offset_in_block);
    //printf("inode_type=0x%04x at offset %u\n", *inode_type, *offset_in_block);
    *offset_in_block += sizeof(uint16_t);

    if (*offset_in_block + sizeof(squash_base_inode_t) - sizeof(uint16_t) > uncompressed_size) {
        fprintf(stderr, "ERROR: offset_in_block=%u > uncompressed_size=%zu (need +%zu for base)\n", 
               *offset_in_block, uncompressed_size, sizeof(squash_base_inode_t) - sizeof(uint16_t));
        return SQUASH_ERROR_INVALID_INODE;
    }
    
    memcpy(base, uncompressed_data + *offset_in_block, sizeof(squash_base_inode_t) - sizeof(uint16_t));
    base->inode_type = *inode_type;
    *offset_in_block += sizeof(squash_base_inode_t) - sizeof(uint16_t);
    
   // printf("After parsing base: offset_in_block=%u\n", *offset_in_block);
    return SQUASH_OK;
}

// Парсер каталогов inode
static squash_error_t parse_dir_inode(
    const squash_base_inode_t *base, const uint8_t *uncompressed_data, size_t uncompressed_size,
    uint32_t *offset_in_block, void **out_inode)
{
    if (*offset_in_block + (SQUASHFS_DIR_INODE_SIZE - sizeof(squash_base_inode_t)) > uncompressed_size)
        return SQUASH_ERROR_INVALID_INODE;
    squash_dir_inode_t *dir_inode = malloc(sizeof(squash_dir_inode_t));
    if (!dir_inode)
        return SQUASH_ERROR_MEMORY;
    memcpy(&dir_inode->base, base, sizeof(squash_base_inode_t));
    memcpy((uint8_t *)dir_inode + sizeof(squash_base_inode_t),
           uncompressed_data + *offset_in_block, SQUASHFS_DIR_INODE_SIZE - sizeof(squash_base_inode_t));
    *offset_in_block += (SQUASHFS_DIR_INODE_SIZE - sizeof(squash_base_inode_t));
    *out_inode = dir_inode;
    return SQUASH_OK;
}

// Парсер регулярного файла
static squash_error_t parse_reg_inode(
    squash_fs_t *fs, const squash_base_inode_t *base, const uint8_t *uncompressed_data, size_t uncompressed_size,
    uint32_t *offset_in_block, void **out_inode)
{
    struct squash_reg_inode_file_t
    {
        uint32_t start_block, fragment, offset, file_size;
    } file;
    if (*offset_in_block + sizeof(file) > uncompressed_size)
        return SQUASH_ERROR_INVALID_INODE;
    memcpy(&file, uncompressed_data + *offset_in_block, sizeof(file));
    *offset_in_block += sizeof(file);

    squash_reg_inode_t *reg_inode = malloc(sizeof(squash_reg_inode_t));
    if (!reg_inode)
        return SQUASH_ERROR_MEMORY;
    memcpy(&reg_inode->base, base, sizeof(squash_base_inode_t));
    reg_inode->start_block = file.start_block;
    reg_inode->fragment = file.fragment;
    reg_inode->offset = file.offset;
    reg_inode->file_size = file.file_size;

    if (reg_inode->start_block >= fs->super.bytes_used)
    {
        free(reg_inode);
        return SQUASH_ERROR_INVALID_INODE;
    }

    uint32_t block_count = 0;
    bool file_in_fragment_only = (reg_inode->fragment != 0xFFFFFFFF &&
                                  reg_inode->file_size <= fs->super.block_size);

    if (file_in_fragment_only)
    {
        reg_inode->block_list = NULL;
    }
    else
    {
        block_count = (reg_inode->file_size + fs->super.block_size - 1) / fs->super.block_size;
        if (reg_inode->fragment != 0xFFFFFFFF && reg_inode->file_size % fs->super.block_size != 0)
        {
            block_count--;
        }

        if (block_count > 0)
        {
            size_t blocks_data_size = block_count * sizeof(uint32_t);
            if (*offset_in_block + blocks_data_size > uncompressed_size)
            {
                fprintf(stderr, "Not enough data for block_list: need %zu bytes, available %zu\n",
                        blocks_data_size, uncompressed_size - *offset_in_block);
                free(reg_inode);
                return SQUASH_ERROR_INVALID_INODE;
            }
            reg_inode->block_list = malloc(blocks_data_size);
            if (!reg_inode->block_list)
            {
                free(reg_inode);
                return SQUASH_ERROR_MEMORY;
            }
            memcpy(reg_inode->block_list, uncompressed_data + *offset_in_block, blocks_data_size);
            *offset_in_block += blocks_data_size;

            fprintf(stderr, "Parsed block_list for inode, block_count=%u:\n", block_count);
            for (uint32_t i = 0; i < block_count; i++)
            {
                uint32_t compressed_size = reg_inode->block_list[i] & ((1 << 24) - 1);
                bool is_compressed = !(reg_inode->block_list[i] & (1 << 24));
                fprintf(stderr, "block_list[%u]=0x%08X (compressed_size=%u, compressed=%d)\n",
                        i, reg_inode->block_list[i], compressed_size, is_compressed);
            }
        }
        else
        {
            reg_inode->block_list = NULL;
        }
    }
    *out_inode = reg_inode;
    return SQUASH_OK;
}

// Парсер расширенного регулярного файла (long regular inode)
static squash_error_t parse_lreg_inode(
    squash_fs_t *fs, const squash_base_inode_t *base, const uint8_t *uncompressed_data, size_t uncompressed_size,
    uint32_t *offset_in_block, void **out_inode)
{
    struct squash_reg_inode_ext
    {
        uint64_t start_block, file_size, sparse;
        uint32_t nlink, fragment, offset, xattr_idx;
    } file_ext;
    if (*offset_in_block + sizeof(file_ext) > uncompressed_size)
        return SQUASH_ERROR_INVALID_INODE;
    memcpy(&file_ext, uncompressed_data + *offset_in_block, sizeof(file_ext));
    *offset_in_block += sizeof(file_ext);

    if (file_ext.fragment != 0xFFFFFFFF && file_ext.fragment >= fs->super.fragments)
        return SQUASH_ERROR_INVALID_INODE;
    if (file_ext.start_block >= fs->super.bytes_used)
        return SQUASH_ERROR_INVALID_INODE;

    squash_reg_inode_t *reg_inode = malloc(sizeof(squash_reg_inode_t));
    if (!reg_inode)
        return SQUASH_ERROR_MEMORY;
    memcpy(&reg_inode->base, base, sizeof(squash_base_inode_t));
    reg_inode->start_block = file_ext.start_block;
    reg_inode->file_size = file_ext.file_size;
    reg_inode->fragment = file_ext.fragment;
    reg_inode->offset = file_ext.offset;

    uint32_t block_count = 0;
    bool file_in_fragment_only = (reg_inode->fragment != 0xFFFFFFFF &&
                                  reg_inode->file_size <= fs->super.block_size);

    if (file_in_fragment_only)
    {
        reg_inode->block_list = NULL;
    }
    else
    {
        block_count = (reg_inode->file_size + fs->super.block_size - 1) / fs->super.block_size;
        if (reg_inode->fragment != 0xFFFFFFFF && reg_inode->file_size % fs->super.block_size != 0)
        {
            block_count--;
        }

        if (block_count > 0)
        {
            size_t blocks_data_size = block_count * sizeof(uint32_t);
            if (*offset_in_block + blocks_data_size > uncompressed_size)
            {
                fprintf(stderr, "Not enough data for block_list: need %zu bytes, available %zu\n",
                        blocks_data_size, uncompressed_size - *offset_in_block);
                free(reg_inode);
                return SQUASH_ERROR_INVALID_INODE;
            }
            reg_inode->block_list = malloc(blocks_data_size);
            if (!reg_inode->block_list)
            {
                free(reg_inode);
                return SQUASH_ERROR_MEMORY;
            }
            memcpy(reg_inode->block_list, uncompressed_data + *offset_in_block, blocks_data_size);
            *offset_in_block += blocks_data_size;

            fprintf(stderr, "Parsed block_list for inode, block_count=%u:\n", block_count);
            for (uint32_t i = 0; i < block_count; i++)
            {
                uint32_t compressed_size = reg_inode->block_list[i] & ((1 << 24) - 1);
                bool is_compressed = !(reg_inode->block_list[i] & (1 << 24));
                fprintf(stderr, "block_list[%u]=0x%08X (compressed_size=%u, compressed=%d)\n",
                        i, reg_inode->block_list[i], compressed_size, is_compressed);
            }
        }
        else
        {
            reg_inode->block_list = NULL;
        }
    }
    *out_inode = reg_inode;
    return SQUASH_OK;
}

// Парсер символических ссылок
static squash_error_t parse_symlink_inode(
    const squash_base_inode_t *base, const uint8_t *uncompressed_data, size_t uncompressed_size,
    uint32_t *offset_in_block, void **out_inode)
{
    if (*offset_in_block + sizeof(squash_symlink_inode_t) - sizeof(squash_base_inode_t) > uncompressed_size)
        return SQUASH_ERROR_INVALID_INODE;

    squash_symlink_inode_t *symlink_inode = malloc(sizeof(squash_symlink_inode_t));
    if (!symlink_inode)
        return SQUASH_ERROR_MEMORY;
    memcpy(&symlink_inode->base, base, sizeof(squash_base_inode_t));
    memcpy((uint8_t *)symlink_inode + sizeof(squash_base_inode_t),
           uncompressed_data + *offset_in_block,
           sizeof(squash_symlink_inode_t) - sizeof(squash_base_inode_t));
    *offset_in_block += sizeof(squash_symlink_inode_t) - sizeof(squash_base_inode_t);

    if (*offset_in_block + symlink_inode->target_size > uncompressed_size)
    {
        free(symlink_inode);
        return SQUASH_ERROR_INVALID_INODE;
    }

    symlink_inode->target_path = malloc(symlink_inode->target_size + 1);
    if (!symlink_inode->target_path)
    {
        free(symlink_inode);
        return SQUASH_ERROR_MEMORY;
    }
    memcpy(symlink_inode->target_path, uncompressed_data + *offset_in_block, symlink_inode->target_size);
    symlink_inode->target_path[symlink_inode->target_size] = '\0';
    *offset_in_block += symlink_inode->target_size;
    *out_inode = symlink_inode;
    return SQUASH_OK;
}

// Парсер блокового устройства
static squash_error_t parse_blkdev_inode(
    const squash_base_inode_t *base, const uint8_t *uncompressed_data, size_t uncompressed_size,
    uint32_t *offset_in_block, void **out_inode)
{
    struct squash_dev_inode_t
    {
        uint32_t nlink;
        uint32_t rdev;
    } dev;
    if (*offset_in_block + sizeof(dev) > uncompressed_size)
        return SQUASH_ERROR_INVALID_INODE;
    memcpy(&dev, uncompressed_data + *offset_in_block, sizeof(dev));
    *offset_in_block += sizeof(dev);

    squash_dev_inode_t *blkdev_inode = malloc(sizeof(squash_dev_inode_t));
    if (!blkdev_inode)
        return SQUASH_ERROR_MEMORY;
    memcpy(&blkdev_inode->base, base, sizeof(squash_base_inode_t));
    blkdev_inode->nlink = dev.nlink;
    blkdev_inode->rdev = dev.rdev;
    *out_inode = blkdev_inode;
    return SQUASH_OK;
}

// Парсер символьного устройства
static squash_error_t parse_chrdev_inode(
    const squash_base_inode_t *base, const uint8_t *uncompressed_data, size_t uncompressed_size,
    uint32_t *offset_in_block, void **out_inode)
{
    struct squash_dev_inode_t
    {
        uint32_t nlink;
        uint32_t rdev;
    } dev;
    if (*offset_in_block + sizeof(dev) > uncompressed_size)
        return SQUASH_ERROR_INVALID_INODE;
    memcpy(&dev, uncompressed_data + *offset_in_block, sizeof(dev));
    *offset_in_block += sizeof(dev);

    squash_dev_inode_t *chrdev_inode = malloc(sizeof(squash_dev_inode_t));
    if (!chrdev_inode)
        return SQUASH_ERROR_MEMORY;
    memcpy(&chrdev_inode->base, base, sizeof(squash_base_inode_t));
    chrdev_inode->nlink = dev.nlink;
    chrdev_inode->rdev = dev.rdev;
    *out_inode = chrdev_inode;
    return SQUASH_OK;
}

// Парсер FIFO (именованного канала)
static squash_error_t parse_fifo_inode(
    const squash_base_inode_t *base, const uint8_t *uncompressed_data, size_t uncompressed_size,
    uint32_t *offset_in_block, void **out_inode)
{
    struct squash_ipc_inode_t
    {
        uint32_t nlink;
    } ipc;
    if (*offset_in_block + sizeof(ipc) > uncompressed_size)
        return SQUASH_ERROR_INVALID_INODE;
    memcpy(&ipc, uncompressed_data + *offset_in_block, sizeof(ipc));
    *offset_in_block += sizeof(ipc);

    squash_ipc_inode_t *fifo_inode = malloc(sizeof(squash_ipc_inode_t));
    if (!fifo_inode)
        return SQUASH_ERROR_MEMORY;
    memcpy(&fifo_inode->base, base, sizeof(squash_base_inode_t));
    fifo_inode->nlink = ipc.nlink;
    *out_inode = fifo_inode;
    return SQUASH_OK;
}

// Парсер сокета
static squash_error_t parse_socket_inode(
    const squash_base_inode_t *base, const uint8_t *uncompressed_data, size_t uncompressed_size,
    uint32_t *offset_in_block, void **out_inode)
{
    struct squash_ipc_inode_t
    {
        uint32_t nlink;
    } ipc;
    if (*offset_in_block + sizeof(ipc) > uncompressed_size)
        return SQUASH_ERROR_INVALID_INODE;
    memcpy(&ipc, uncompressed_data + *offset_in_block, sizeof(ipc));
    *offset_in_block += sizeof(ipc);

    squash_ipc_inode_t *socket_inode = malloc(sizeof(squash_ipc_inode_t));
    if (!socket_inode)
        return SQUASH_ERROR_MEMORY;
    memcpy(&socket_inode->base, base, sizeof(squash_base_inode_t));
    socket_inode->nlink = ipc.nlink;
    *out_inode = socket_inode;
    return SQUASH_OK;
}

// Парсер расширенного блокового устройства
static squash_error_t parse_lblkdev_inode(
    const squash_base_inode_t *base, const uint8_t *uncompressed_data, size_t uncompressed_size,
    uint32_t *offset_in_block, void **out_inode)
{
    struct squash_ldev_inode_t
    {
        uint32_t nlink;
        uint32_t rdev;
        uint32_t xattr_idx;
    } ldev;
    if (*offset_in_block + sizeof(ldev) > uncompressed_size)
        return SQUASH_ERROR_INVALID_INODE;
    memcpy(&ldev, uncompressed_data + *offset_in_block, sizeof(ldev));
    *offset_in_block += sizeof(ldev);

    squash_dev_inode_t *lblkdev_inode = malloc(sizeof(squash_dev_inode_t));
    if (!lblkdev_inode)
        return SQUASH_ERROR_MEMORY;
    memcpy(&lblkdev_inode->base, base, sizeof(squash_base_inode_t));
    lblkdev_inode->nlink = ldev.nlink;
    lblkdev_inode->rdev = ldev.rdev;
    *out_inode = lblkdev_inode;
    return SQUASH_OK;
}

// Парсер расширенного символьного устройства
static squash_error_t parse_lchrdev_inode(
    const squash_base_inode_t *base, const uint8_t *uncompressed_data, size_t uncompressed_size,
    uint32_t *offset_in_block, void **out_inode)
{
    struct squash_ldev_inode_t
    {
        uint32_t nlink;
        uint32_t rdev;
        uint32_t xattr_idx;
    } ldev;
    if (*offset_in_block + sizeof(ldev) > uncompressed_size)
        return SQUASH_ERROR_INVALID_INODE;
    memcpy(&ldev, uncompressed_data + *offset_in_block, sizeof(ldev));
    *offset_in_block += sizeof(ldev);

    squash_dev_inode_t *lchrdev_inode = malloc(sizeof(squash_dev_inode_t));
    if (!lchrdev_inode)
        return SQUASH_ERROR_MEMORY;
    memcpy(&lchrdev_inode->base, base, sizeof(squash_base_inode_t));
    lchrdev_inode->nlink = ldev.nlink;
    lchrdev_inode->rdev = ldev.rdev;
    *out_inode = lchrdev_inode;
    return SQUASH_OK;
}

// Парсер расширенного FIFO
static squash_error_t parse_lfifo_inode(
    const squash_base_inode_t *base, const uint8_t *uncompressed_data, size_t uncompressed_size,
    uint32_t *offset_in_block, void **out_inode)
{
    struct squash_lipc_inode_t
    {
        uint32_t nlink;
        uint32_t xattr_idx;
    } lipc;
    if (*offset_in_block + sizeof(lipc) > uncompressed_size)
        return SQUASH_ERROR_INVALID_INODE;
    memcpy(&lipc, uncompressed_data + *offset_in_block, sizeof(lipc));
    *offset_in_block += sizeof(lipc);

    squash_ipc_inode_t *lfifo_inode = malloc(sizeof(squash_ipc_inode_t));
    if (!lfifo_inode)
        return SQUASH_ERROR_MEMORY;
    memcpy(&lfifo_inode->base, base, sizeof(squash_base_inode_t));
    lfifo_inode->nlink = lipc.nlink;
    *out_inode = lfifo_inode;
    return SQUASH_OK;
}

// Парсер расширенного сокета
static squash_error_t parse_lsocket_inode(
    const squash_base_inode_t *base, const uint8_t *uncompressed_data, size_t uncompressed_size,
    uint32_t *offset_in_block, void **out_inode)
{
    struct squash_lipc_inode_t
    {
        uint32_t nlink;
        uint32_t xattr_idx;
    } lipc;
    if (*offset_in_block + sizeof(lipc) > uncompressed_size)
        return SQUASH_ERROR_INVALID_INODE;
    memcpy(&lipc, uncompressed_data + *offset_in_block, sizeof(lipc));
    *offset_in_block += sizeof(lipc);

    squash_ipc_inode_t *lsocket_inode = malloc(sizeof(squash_ipc_inode_t));
    if (!lsocket_inode)
        return SQUASH_ERROR_MEMORY;
    memcpy(&lsocket_inode->base, base, sizeof(squash_base_inode_t));
    lsocket_inode->nlink = lipc.nlink;
    *out_inode = lsocket_inode;
    return SQUASH_OK;
}

// Главная публичная функция
SQUASH_API squash_error_t squash_read_inode(squash_fs_t *fs, squash_off_t inode_ref, void **inode)
{
    if (!fs || !fs->file || !inode || !fs->decompressor)
    {
        return SQUASH_ERROR_INVALID_FILE;
    }

    uint64_t block_offset;
    uint32_t offset_in_block;
    parse_inode_ref(inode_ref, &block_offset, &offset_in_block);

    uint8_t *uncompressed_data = NULL;
    size_t uncompressed_size = 0;
    squash_error_t err = load_inode_metablock(fs, block_offset, &uncompressed_data, &uncompressed_size);
    if (err != SQUASH_OK)
        return err;

    uint8_t *final_data = uncompressed_data;
    size_t final_size = uncompressed_size;
    bool need_merge = false;

    // Проверяем, достаточно ли места для минимального inode (base + type-specific)
    size_t min_required = sizeof(uint16_t) + sizeof(squash_base_inode_t) - sizeof(uint16_t) + 32; // +32 на type-specific данные
    
    if (offset_in_block + min_required > uncompressed_size) {
        need_merge = true;
        
        // Читаем следующий метаблок
        uint8_t *next_data = NULL;
        size_t next_size = 0;
        
        // Ищем следующий метаблок
        uint64_t next_block_offset = block_offset;
        uint64_t current_metablock_start = fs->super.inode_table_start + block_offset;
        
        // Переходим к следующему метаблоку (current + header + compressed_size)
        if (fseek(fs->file, current_metablock_start, SEEK_SET) != 0) {
            free(uncompressed_data);
            return SQUASH_ERROR_IO;
        }
        
        uint16_t header;
        if (fread(&header, sizeof(uint16_t), 1, fs->file) != 1) {
            free(uncompressed_data);
            return SQUASH_ERROR_IO;
        }
        
        uint16_t compressed_size = header & 0x7FFF;
        next_block_offset = block_offset + 2 + compressed_size;
        
        err = load_inode_metablock(fs, next_block_offset, &next_data, &next_size);
        if (err != SQUASH_OK) {
            free(uncompressed_data);
            return err;
        }
        
        // Создаем объединенный буфер
        final_size = uncompressed_size + next_size;
        final_data = malloc(final_size);
        if (!final_data) {
            free(uncompressed_data);
            free(next_data);
            return SQUASH_ERROR_MEMORY;
        }
        
        memcpy(final_data, uncompressed_data, uncompressed_size);
        memcpy(final_data + uncompressed_size, next_data, next_size);
        
        free(uncompressed_data);
        free(next_data);
        
        //printf("offset_in_block=%u, original_size=%zu, merged_size=%zu\n", 
               //offset_in_block, uncompressed_size, final_size);
    }

    squash_base_inode_t base;
    uint16_t inode_type;
    err = parse_base_inode(final_data, final_size, &offset_in_block, &base, &inode_type);
    if (err != SQUASH_OK)
    {
        free(final_data);
        return err;
    }

    void *result_inode = NULL;
    switch (inode_type)
    {
    case SQUASHFS_DIR_TYPE:
    case SQUASHFS_LDIR_TYPE:
        err = parse_dir_inode(&base, final_data, final_size, &offset_in_block, &result_inode);
        break;
    case SQUASHFS_REG_TYPE:
        err = parse_reg_inode(fs, &base, final_data, final_size, &offset_in_block, &result_inode);
        break;
    case SQUASHFS_LREG_TYPE:
        err = parse_lreg_inode(fs, &base, final_data, final_size, &offset_in_block, &result_inode);
        break;
    case SQUASHFS_SYMLINK_TYPE:
    case SQUASHFS_LSYMLINK_TYPE:
        err = parse_symlink_inode(&base, final_data, final_size, &offset_in_block, &result_inode);
        break;
    case SQUASHFS_BLKDEV_TYPE:
        err = parse_blkdev_inode(&base, final_data, final_size, &offset_in_block, &result_inode);
        break;
    case SQUASHFS_CHRDEV_TYPE:
        err = parse_chrdev_inode(&base, final_data, final_size, &offset_in_block, &result_inode);
        break;
    case SQUASHFS_FIFO_TYPE:
        err = parse_fifo_inode(&base, final_data, final_size, &offset_in_block, &result_inode);
        break;
    case SQUASHFS_SOCKET_TYPE:
        err = parse_socket_inode(&base, final_data, final_size, &offset_in_block, &result_inode);
        break;
    case SQUASHFS_LBLKDEV_TYPE:
        err = parse_lblkdev_inode(&base, final_data, final_size, &offset_in_block, &result_inode);
        break;
    case SQUASHFS_LCHRDEV_TYPE:
        err = parse_lchrdev_inode(&base, final_data, final_size, &offset_in_block, &result_inode);
        break;
    case SQUASHFS_LFIFO_TYPE:
        err = parse_lfifo_inode(&base, final_data, final_size, &offset_in_block, &result_inode);
        break;
    case SQUASHFS_LSOCKET_TYPE:
        err = parse_lsocket_inode(&base, final_data, final_size, &offset_in_block, &result_inode);
        break;
    default:
        err = SQUASH_ERROR_INVALID_INODE;
    }

    free(final_data);
    if (err == SQUASH_OK)
        *inode = result_inode;
    return err;
}

SQUASH_API void squash_free_inode(void *inode)
{
    if (!inode)
        return;
    squash_base_inode_t *base = (squash_base_inode_t *)inode;
    if (base->inode_type == SQUASHFS_REG_TYPE || base->inode_type == SQUASHFS_LREG_TYPE)
    {
        squash_reg_inode_t *reg_inode = (squash_reg_inode_t *)inode;
        if (reg_inode->block_list)
        {
            free(reg_inode->block_list);
        }
    }
    else if (base->inode_type == SQUASHFS_SYMLINK_TYPE || base->inode_type == SQUASHFS_LSYMLINK_TYPE)
    {
        squash_symlink_inode_t *symlink_inode = (squash_symlink_inode_t *)inode;
        if (symlink_inode->target_path)
        {
            free(symlink_inode->target_path);
        }
    }
    free(inode);
}