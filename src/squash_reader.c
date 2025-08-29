#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/libsquash/squash.h"

#define SQUASHFS_MAGIC 0x73717368
#define SQUASHFS_VERSION_MAJOR 4
#define SQUASHFS_INVALID_BLK 0xFFFFFFFFFFFFFFFF

static squash_error_t read_super_block(FILE *file, squash_super_t *super)
{
    if (fseek(file, 0, SEEK_SET) != 0)
    {
        fprintf(stderr, "Error seeking to start of file: %s\n", strerror(errno));
        return SQUASH_ERROR_IO;
    }

    // Читаем сырые данные суперблока (96 байт)
    uint8_t raw_super[96];
    if (fread(raw_super, 1, sizeof(raw_super), file) != sizeof(raw_super))
    {
        fprintf(stderr, "Error reading superblock: %s\n", strerror(errno));
        return SQUASH_ERROR_IO;
    }

    // Парсим поля без преобразования порядка байт
    super->s_magic = *(uint32_t *)(raw_super + 0);
    super->inodes = *(uint32_t *)(raw_super + 4);
    super->mkfs_time = *(uint32_t *)(raw_super + 8);
    super->block_size = *(uint32_t *)(raw_super + 12);
    super->fragments = *(uint32_t *)(raw_super + 16);
    super->compression = *(uint16_t *)(raw_super + 20);
    super->block_log = *(uint16_t *)(raw_super + 22);
    super->flags = *(uint16_t *)(raw_super + 24);
    super->no_ids = *(uint16_t *)(raw_super + 26);
    super->s_major = *(uint16_t *)(raw_super + 28);
    super->s_minor = *(uint16_t *)(raw_super + 30);
    super->root_inode = *(uint64_t *)(raw_super + 32);
    super->bytes_used = *(uint64_t *)(raw_super + 40);
    super->id_table_start = *(uint64_t *)(raw_super + 48);
    super->xattr_id_table_start = *(uint64_t *)(raw_super + 56);
    super->inode_table_start = *(uint64_t *)(raw_super + 64);
    super->directory_table_start = *(uint64_t *)(raw_super + 72);
    super->fragment_table_start = *(uint64_t *)(raw_super + 80);
    super->lookup_table_start = *(uint64_t *)(raw_super + 88);

    // Проверяем magic
    if (super->s_magic != SQUASHFS_MAGIC)
    {
        fprintf(stderr, "Invalid magic: expected 0x%08X, got 0x%08X\n", SQUASHFS_MAGIC, super->s_magic);
        // Отладка: выводим первые 4 байта
        fprintf(stderr, "Raw magic bytes: %02X %02X %02X %02X\n",
               raw_super[0], raw_super[1], raw_super[2], raw_super[3]);
        return SQUASH_ERROR_INVALID_MAGIC;
    }

    // Проверяем версию
    if (super->s_major != SQUASHFS_VERSION_MAJOR || super->s_minor > 1)
    {
        fprintf(stderr, "Unsupported version: %u.%u\n", super->s_major, super->s_minor);
        return SQUASH_ERROR_UNSUPPORTED_VERSION;
    }

    // Проверяем inode_table_start
    if (super->inode_table_start >= super->bytes_used)
    {
        fprintf(stderr, "Invalid inode_table_start: 0x%llX >= bytes_used: 0x%llX\n",
               super->inode_table_start, super->bytes_used);
        return SQUASH_ERROR_INVALID_FILE;
    }

    // Проверяем compression
    if (super->compression < 1 || super->compression > 5)
    {
        fprintf(stderr, "Unsupported compression: %u\n", super->compression);
        return SQUASH_ERROR_COMPRESSION;
    }

    // Проверяем block_size
    if (super->block_size != (1u << super->block_log))
    {
        fprintf(stderr, "Invalid block_size: %u, expected %u (block_log=%u)\n",
               super->block_size, 1u << super->block_log, super->block_log);
        return SQUASH_ERROR_INVALID_FILE;
    }

    printf("Superblock loaded successfully:\n");
    printf("  Magic: 0x%08X\n", super->s_magic);
    printf("  Version: %u.%u\n", super->s_major, super->s_minor);
    printf("  Inodes: %u\n", super->inodes);
    printf("  Bytes used: %llu\n", super->bytes_used);
    printf("  Root inode: 0x%016llX\n", super->root_inode);
    printf("  Compression: %u\n", super->compression);
    printf("  Block size: %u\n", super->block_size);
    printf("  Inode table start: 0x%016llX\n", super->inode_table_start);
    printf("DEBUG: root_inode raw = 0x%016llX\n", super->root_inode);
    printf("DEBUG: root_inode block = %llu\n", super->root_inode >> 16);
    printf("DEBUG: root_inode offset = %u\n", (uint16_t)(super->root_inode & 0xFFFF));
    printf("DEBUG: inode_table_start = 0x%llX\n", super->inode_table_start);
    printf("DEBUG: directory_table_start = 0x%llX\n", super->directory_table_start);

    // Отладочный вывод сырых данных
   /* printf("Raw superblock data:\n");
    for (int i = 0; i < sizeof(raw_super); i++)
    {
        printf("%02X ", raw_super[i]);
        if ((i + 1) % 16 == 0)
            printf("\n");
    }
    printf("\n");*/

    return SQUASH_OK;
}

#define SQUASHFS_LOOKUP_BYTES(inodes) ((inodes) * sizeof(uint64_t))
#define SQUASHFS_LOOKUP_BLOCKS(inodes) (((inodes) * sizeof(uint64_t) + SQUASHFS_METADATA_SIZE - 1) / SQUASHFS_METADATA_SIZE)
#define SQUASHFS_LOOKUP_BLOCK_BYTES(inodes) (SQUASHFS_LOOKUP_BLOCKS(inodes) * sizeof(uint64_t))

// Функция для чтения блока с заданным ожидаемым размером
static int is_little_endian(void)
{
    union
    {
        uint16_t u16;
        uint8_t bytes[2];
    } test = {.u16 = 0x1234};
    return test.bytes[0] == 0x34; // Если младший байт первый, то little-endian
}


// Разворот uint64_t (big-endian <-> little-endian)
static void inswap_long_longs(uint64_t *data, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        uint64_t v = data[i];
        data[i] = ((v & 0xFFULL) << 56) |
                  ((v & 0xFF00ULL) << 40) |
                  ((v & 0xFF0000ULL) << 24) |
                  ((v & 0xFF000000ULL) << 8) |
                  ((v & 0xFF00000000ULL) >> 8) |
                  ((v & 0xFF0000000000ULL) >> 24) |
                  ((v & 0xFF000000000000ULL) >> 40) |
                  ((v & 0xFF00000000000000ULL) >> 56);
    }
}

// Разворот uint16_t (big-endian <-> little-endian)
static void inswap_shorts(uint16_t *data, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        uint16_t v = data[i];
        data[i] = (uint16_t)((v << 8) | (v >> 8));
    }
}

static squash_error_t read_inode_lookup_table(squash_fs_t *fs)
{
    squash_super_t *super = &fs->super;

    if (super->lookup_table_start == SQUASHFS_INVALID_BLK) {
        printf("No lookup table present\n");
        fs->inode_lookup_table = NULL;
        return SQUASH_OK;
    }

    // Отладочный вывод
    /*printf("Lookup table debug:\n");
    printf("  lookup_table_start: 0x%llX (%llu)\n", super->lookup_table_start, super->lookup_table_start);
    printf("  inodes: %u\n", super->inodes);
    printf("  bytes_used: 0x%llX (%llu)\n", super->bytes_used, super->bytes_used);
    printf("  inode_table_start: 0x%llX (%llu)\n", super->inode_table_start, super->inode_table_start);
    printf("  directory_table_start: 0x%llX (%llu)\n", super->directory_table_start, super->directory_table_start);*/

    uint32_t inodes = super->inodes;
    size_t lookup_bytes = SQUASHFS_LOOKUP_BYTES(inodes);
    uint32_t lookup_blocks = SQUASHFS_LOOKUP_BLOCKS(inodes);
    size_t index_bytes = SQUASHFS_LOOKUP_BLOCK_BYTES(inodes);

    /*printf("  lookup_bytes: %zu\n", lookup_bytes);
    printf("  lookup_blocks: %u\n", lookup_blocks);
    printf("  index_bytes: %zu\n", index_bytes);*/

    // Проверка lookup_table_start
    if (super->lookup_table_start >= super->bytes_used) {
        fprintf(stderr, "Invalid lookup_table_start: 0x%llX >= bytes_used: 0x%llX\n",
               super->lookup_table_start, super->bytes_used);
        fs->inode_lookup_table = NULL;
        return SQUASH_OK;
    }

    // Выделяем память под индексы
    uint64_t *block_index = malloc(index_bytes);
    if (!block_index) {
        fprintf(stderr, "Memory allocation failed for block index\n");
        return SQUASH_ERROR_MEMORY;
    }

    // Читаем индексы блоков
    if (read_fs_bytes(fs->file, super->lookup_table_start, index_bytes, block_index) != SQUASH_OK) {
        fprintf(stderr, "Failed to read inode lookup table index at 0x%llX\n", super->lookup_table_start);
        free(block_index);
        return SQUASH_ERROR_IO;
    }


    // Вывод индексов
   /* printf("Lookup table block indices:\n");
    for (uint32_t i = 0; i < lookup_blocks; i++) {
        printf("  index[%u] = 0x%llX (%llu)\n", i, block_index[i], block_index[i]);
    }*/

    // Проверяем валидность индексов
    bool valid_indices = true;
    for (uint32_t i = 0; i < lookup_blocks; i++) {
        if (block_index[i] >= super->bytes_used || block_index[i] < super->inode_table_start) {
            fprintf(stderr, "Invalid block index[%u]: 0x%llX (out of range 0x%llX - 0x%llX)\n",
                   i, block_index[i], super->inode_table_start, super->bytes_used);
            valid_indices = false;
            break;
        }
    }

    if (!valid_indices) {
        fprintf(stderr, "Invalid lookup table indices, ignoring lookup table\n");
        free(block_index);
        fs->inode_lookup_table = NULL;
        return SQUASH_OK;
    }

    // Выделяем память под таблицу lookup
    fs->inode_lookup_table = calloc(inodes, sizeof(uint64_t));
    if (!fs->inode_lookup_table) {
        fprintf(stderr, "Memory allocation failed for inode lookup table\n");
        free(block_index);
        return SQUASH_ERROR_MEMORY;
    }

    // Читаем и распаковываем блоки
    size_t copied = 0;
    uint64_t max_valid_offset = super->directory_table_start - super->inode_table_start;
    //printf("  max_valid_offset: 0x%llX (%llu)\n", max_valid_offset, max_valid_offset);

    for (uint32_t i = 0; i < lookup_blocks; i++) {
        size_t expected = SQUASHFS_METADATA_SIZE;
        if (i == lookup_blocks - 1 && (lookup_bytes % SQUASHFS_METADATA_SIZE)) {
            expected = lookup_bytes % SQUASHFS_METADATA_SIZE;
        }

        //printf("Reading lookup block %u from 0x%llX, expected size: %zu\n", i, block_index[i], expected);

        uint16_t block_header;
        if (read_fs_bytes(fs->file, block_index[i], 2, &block_header) != SQUASH_OK) {
            fprintf(stderr, "Failed to read block header at 0x%llX\n", block_index[i]);
            free(block_index);
            free(fs->inode_lookup_table);
            fs->inode_lookup_table = NULL;
            return SQUASH_ERROR_IO;
        }

        bool is_compressed = !(block_header & SQUASHFS_COMPRESSED_BIT_BLOCK);
        uint16_t block_size = block_header & SQUASHFS_COMPRESSED_SIZE_MASK;

        /*printf("Block %u: header=0x%04X, compressed=%s, size=%u\n",
               i, block_header, is_compressed ? "yes" : "no", block_size);*/

        if (block_size == 0 || block_size > SQUASHFS_METADATA_SIZE) {
            fprintf(stderr, "Invalid block size %u at offset 0x%llX\n", block_size, block_index[i]);
            free(block_index);
            free(fs->inode_lookup_table);
            fs->inode_lookup_table = NULL;
            return SQUASH_ERROR_IO;
        }

        uint8_t *compressed_data = malloc(block_size);
        if (!compressed_data) {
            fprintf(stderr, "Memory allocation failed for compressed data\n");
            free(block_index);
            free(fs->inode_lookup_table);
            fs->inode_lookup_table = NULL;
            return SQUASH_ERROR_MEMORY;
        }

        if (read_fs_bytes(fs->file, block_index[i] + 2, block_size, compressed_data)!=SQUASH_OK) {
            fprintf(stderr, "Failed to read compressed data at 0x%llX\n", block_index[i] + 2);
            free(compressed_data);
            free(block_index);
            free(fs->inode_lookup_table);
            fs->inode_lookup_table = NULL;
            return SQUASH_ERROR_IO;
        }

        size_t uncompressed_size = expected;
        uint8_t *uncompressed_data = malloc(uncompressed_size);
        if (!uncompressed_data) {
            fprintf(stderr, "Memory allocation failed for uncompressed data\n");
            free(compressed_data);
            free(block_index);
            free(fs->inode_lookup_table);
            fs->inode_lookup_table = NULL;
            return SQUASH_ERROR_MEMORY;
        }

        if (is_compressed) {
            squash_error_t err = squash_decompress_block(fs->decompressor, compressed_data, block_size,
                                                        uncompressed_data, &uncompressed_size);
            free(compressed_data);
            if (err != SQUASH_OK) {
                fprintf(stderr, "Failed to decompress lookup block: %s\n", squash_strerror(err));
                free(uncompressed_data);
                free(block_index);
                free(fs->inode_lookup_table);
                fs->inode_lookup_table = NULL;
                return err;
            }
        } else {
            memcpy(uncompressed_data, compressed_data, block_size);
            uncompressed_size = block_size;
            free(compressed_data);
        }

        // Копируем записи
        size_t entries_count = uncompressed_size / sizeof(uint64_t);
        uint64_t *entries = (uint64_t *)uncompressed_data;
        for (size_t j = 0; j < entries_count && copied < lookup_bytes; j++) {
            fs->inode_lookup_table[copied / sizeof(uint64_t)] = entries[j];
            copied += sizeof(uint64_t);
        }

        // Вывод первых байтов раcпакованного блока
       /* printf("Block %u raw data (first 24 bytes):\n");
        for (size_t j = 0; j < 24 && j < uncompressed_size; j++) {
            printf("%02X ", uncompressed_data[j]);
            if ((j + 1) % 8 == 0) printf("\n");
        }
        printf("\n");*/

        free(uncompressed_data);
    }

    free(block_index);

    // Проверяем валидность записей
    /*uint32_t valid_entries = 0;
    for (uint32_t i = 0; i < inodes; i++) {
        uint64_t inode_offset = fs->inode_lookup_table[i];
        if (inode_offset >= max_valid_offset) {
           // printf("Invalid inode offset at index %u: 0x%llX >= max_valid_offset: 0x%llX\n",
           //        i, inode_offset, max_valid_offset);
            fs->inode_lookup_table[i] = 0;
        } else {
            valid_entries++;
        }
    }*/

    // Если все записи невалидны, игнорируем таблицу
    /*if (valid_entries == 0) {
        printf("No valid entries in lookup table, ignoring\n");
        free(fs->inode_lookup_table);
        fs->inode_lookup_table = NULL;
    } else {
        printf("Successfully read inode lookup table with %u entries (%u valid)\n", inodes, valid_entries);
    }*/

    return SQUASH_OK;
}


static squash_error_t init_decompressor(squash_fs_t *fs)
{
    switch (fs->super.compression)
    {
    case SQUASH_COMPRESSION_GZIP:
        fs->decompressor = squash_decompressor_create(SQUASH_COMPRESSION_GZIP);
        //printf("GZIP decompressor initialized\n");
        break;
    case SQUASH_COMPRESSION_LZMA:
        fs->decompressor = squash_decompressor_create(SQUASH_COMPRESSION_LZMA);
        break;
    case SQUASH_COMPRESSION_LZO:
        fs->decompressor = squash_decompressor_create(SQUASH_COMPRESSION_LZO);
        break;
    case SQUASH_COMPRESSION_XZ:
        fs->decompressor = squash_decompressor_create(SQUASH_COMPRESSION_XZ);
        break;
    case SQUASH_COMPRESSION_LZ4:
        fs->decompressor = squash_decompressor_create(SQUASH_COMPRESSION_LZ4);
        break;
    default:
        return SQUASH_ERROR_COMPRESSION;
    }

    if (!fs->decompressor)
    {
        fprintf(stderr, "Error creating decompressor for compression type %u\n", fs->super.compression);
        return SQUASH_ERROR_COMPRESSION;
    }

    return SQUASH_OK;
}

static squash_error_t find_root_inode(squash_fs_t *fs)
{
    uint64_t start = fs->super.inode_table_start;
    uint64_t end = fs->super.directory_table_start;
    uint64_t root_inode_start = start + (fs->super.root_inode >> 16); // Byte offset to block
    uint32_t root_inode_offset = fs->super.root_inode & 0xFFFF;       // Offset in block
    uint32_t current_block_index = 0;

    if (fseek(fs->file, start, SEEK_SET) != 0)
    {
        printf("Failed to seek to inode table start: %s\n", strerror(errno));
        return SQUASH_ERROR_IO;
    }

    while (start < end)
    {
        long current_file_offset = ftell(fs->file);
        if (current_file_offset < 0 || (uint64_t)current_file_offset >= fs->super.bytes_used)
        {
            fprintf(stderr, "Current offset %ld exceeds bytes_used %llu\n", current_file_offset, fs->super.bytes_used);
            return SQUASH_ERROR_IO;
        }

        uint16_t block_header;
        if (fread(&block_header, sizeof(uint16_t), 1, fs->file) != 1)
        {
            fprintf(stderr, "Failed to read block header: %s\n", strerror(errno));
            return SQUASH_ERROR_IO;
        }

        bool is_compressed = !(block_header & SQUASHFS_COMPRESSED_BIT_BLOCK);
        uint16_t block_size = block_header & SQUASHFS_COMPRESSED_SIZE_MASK;
        //printf("Scanning block %u at 0x%lx: header=0x%04X, compressed=%s, size=%u\n",
              // current_block_index, current_file_offset, block_header, is_compressed ? "yes" : "no", block_size);

        if (block_size == 0 || block_size > SQUASHFS_METADATA_SIZE || (uint64_t)current_file_offset + block_size > fs->super.bytes_used)
        {
            fprintf(stderr, "Invalid block size %u at offset %ld\n", block_size, current_file_offset);
            return SQUASH_ERROR_IO;
        }

        uint8_t *compressed_data = malloc(block_size);
        if (!compressed_data)
        {
            fprintf(stderr, "Memory allocation failed for block size %u\n", block_size);
            return SQUASH_ERROR_MEMORY;
        }
        if (fread(compressed_data, 1, block_size, fs->file) != block_size)
        {
            free(compressed_data);
            fprintf(stderr, "Failed to read block data: %s\n", strerror(errno));
            return SQUASH_ERROR_IO;
        }

        size_t uncompressed_size = SQUASHFS_METADATA_SIZE;
        uint8_t *uncompressed_data = malloc(uncompressed_size);
        if (!uncompressed_data)
        {
            free(compressed_data);
            fprintf(stderr, "Memory allocation failed for uncompressed data\n");
            return SQUASH_ERROR_MEMORY;
        }

        if (is_compressed)
        {
            squash_error_t err = squash_decompress_block(fs->decompressor, compressed_data, block_size,
                                                         uncompressed_data, &uncompressed_size);
            free(compressed_data);
            if (err != SQUASH_OK)
            {
                free(uncompressed_data);
                fprintf(stderr, "Failed to decompress block: %s\n", squash_strerror(err));
                return err;
            }
        }
        else
        {
            memcpy(uncompressed_data, compressed_data, block_size);
            uncompressed_size = block_size;
            free(compressed_data);
        }

        if ((uint64_t)current_file_offset == root_inode_start)
        {
            if (root_inode_offset + sizeof(uint16_t) <= uncompressed_size)
            {
                uint16_t inode_type = *(uint16_t *)(uncompressed_data + root_inode_offset);
                if (inode_type == SQUASHFS_DIR_TYPE || inode_type == SQUASHFS_LDIR_TYPE)
                {
                    uint64_t block_offset = current_file_offset - fs->super.inode_table_start;
                    fs->super.root_inode = (block_offset << 16) | root_inode_offset;
                    //"Found valid root directory at offset 0x%llx, type=%u\n",
                           //root_inode_start, inode_type);
                    free(uncompressed_data);
                    return SQUASH_OK;
                }
                //printf("Root inode at offset 0x%llx is not a directory (type=%u)\n",
                       //root_inode_start, inode_type);
            }
        }

        start = current_file_offset + 2 + block_size;
        current_block_index++;
        free(uncompressed_data);
    }

    fprintf(stderr, "No valid root directory found in inode table\n");
    return SQUASH_ERROR_INVALID_INODE;
}

static squash_error_t read_fragment_table(squash_fs_t *fs)
{
    squash_super_t *super = &fs->super;
    if (super->fragments == 0 || super->fragment_table_start == SQUASHFS_INVALID_BLK)
    {
        fs->fragment_table = NULL;
        fprintf(stderr, "No fragment table present (fragments=%u)\n", super->fragments);
        return SQUASH_OK;
    }

    // Сколько entries помещается в один metadata block
    uint32_t entries_per_block = SQUASHFS_METADATA_SIZE / sizeof(struct squashfs_fragment_entry);
    uint32_t fragment_blocks = (super->fragments + entries_per_block - 1) / entries_per_block;

    //printf("FragmentTable: %u entries, %u entries/block, %u blocks\n",
           //super->fragments, entries_per_block, fragment_blocks);

    // Прочитать индексную таблицу (LE64 адреса блоков fragment_table)
    uint64_t *fragment_index = malloc(fragment_blocks * sizeof(uint64_t));
    if (!fragment_index)
    {
        fprintf(stderr, "Memory allocation failed for fragment_index\n");
        return SQUASH_ERROR_MEMORY;
    }
    if (fseek(fs->file, super->fragment_table_start, SEEK_SET) != 0)
    {
        free(fragment_index);
        fprintf(stderr, "Failed to seek to fragment_table_start\n");
        return SQUASH_ERROR_IO;
    }
    if (fread(fragment_index, sizeof(uint64_t), fragment_blocks, fs->file) != fragment_blocks)
    {
        free(fragment_index);
        fprintf(stderr, "Failed to read fragment_index\n");
        return SQUASH_ERROR_IO;
    }
    // Выделяем память для всех fragment entries сразу
    fs->fragment_table = calloc(super->fragments, sizeof(struct squashfs_fragment_entry));
    if (!fs->fragment_table)
    {
        free(fragment_index);
        fprintf(stderr, "Memory allocation failed for fragment_table\n");
        return SQUASH_ERROR_MEMORY;
    }

    uint32_t fragments_read = 0;
    for (uint32_t block_idx = 0; block_idx < fragment_blocks && fragments_read < super->fragments; block_idx++)
    {
        uint64_t block_offset = fragment_index[block_idx] & 0x7FFFFFFFFFFFFFFFULL;

        // Читаем header
        uint16_t block_header;
        if (fseek(fs->file, block_offset, SEEK_SET) != 0 ||
            fread(&block_header, sizeof(uint16_t), 1, fs->file) != 1)
        {
            free(fragment_index);
            free(fs->fragment_table);
            fs->fragment_table = NULL;
            fprintf(stderr, "Failed to read fragment block header\n");
            return SQUASH_ERROR_IO;
        }
        block_header = block_header;

        bool is_compressed = !(block_header & SQUASHFS_COMPRESSED_BIT_BLOCK); // 0x8000 -> несжатый блок
        uint16_t compressed_size = block_header & SQUASHFS_COMPRESSED_SIZE_MASK;

        if (compressed_size == 0 || compressed_size > SQUASHFS_METADATA_SIZE)
        {
            free(fragment_index);
            free(fs->fragment_table);
            fs->fragment_table = NULL;
            fprintf(stderr, "Invalid fragment block size: %u\n", compressed_size);
            return SQUASH_ERROR_INVALID_FILE;
        }

        uint8_t *compressed_data = malloc(compressed_size);
        if (!compressed_data)
        {
            free(fragment_index);
            free(fs->fragment_table);
            fs->fragment_table = NULL;
            fprintf(stderr, "Malloc failed (compressed fragment data)\n");
            return SQUASH_ERROR_MEMORY;
        }
        if (fread(compressed_data, 1, compressed_size, fs->file) != compressed_size)
        {
            free(compressed_data);
            free(fragment_index);
            free(fs->fragment_table);
            fs->fragment_table = NULL;
            fprintf(stderr, "Failed to read compressed fragment block\n");
            return SQUASH_ERROR_IO;
        }

        // Разжимаем (или копируем) данные блока
        uint8_t decompressed_data[SQUASHFS_METADATA_SIZE];
        size_t decompressed_size = SQUASHFS_METADATA_SIZE;
        squash_error_t err = SQUASH_OK;

        if (is_compressed)
        {
            err = squash_decompress_block(fs->decompressor, compressed_data, compressed_size,
                                          decompressed_data, &decompressed_size);
            if (err != SQUASH_OK)
            {
                free(compressed_data);
                free(fragment_index);
                free(fs->fragment_table);
                fs->fragment_table = NULL;
                fprintf(stderr, "Decompress error: %s\n", squash_strerror(err));
                return err;
            }
            if (decompressed_size > SQUASHFS_METADATA_SIZE)
            {
                free(compressed_data);
                free(fragment_index);
                free(fs->fragment_table);
                fs->fragment_table = NULL;
                fprintf(stderr, "Too big decompressed fragment block: %zu\n", decompressed_size);
                return SQUASH_ERROR_INVALID_FILE;
            }
        }
        else
        {
            memcpy(decompressed_data, compressed_data, compressed_size);
            decompressed_size = compressed_size;
        }
        free(compressed_data);

        // Копируем fragment entries из этого блока
        uint32_t entries_left = super->fragments - fragments_read;
        uint32_t entries_here = decompressed_size / sizeof(struct squashfs_fragment_entry);
        if (entries_here > entries_left)
            entries_here = entries_left;

        struct squashfs_fragment_entry *src = (struct squashfs_fragment_entry *)decompressed_data;
        for (uint32_t i = 0; i < entries_here; ++i)
        {
            fs->fragment_table[fragments_read + i].start_block = src[i].start_block;
            fs->fragment_table[fragments_read + i].size = src[i].size;
            fs->fragment_table[fragments_read + i].unused = src[i].unused;
           // printf("fragment_table[%u]: start_block=0x%llX size=%u unused=0x%X\n",
           //        fragments_read + i, (unsigned long long)fs->fragment_table[fragments_read + i].start_block,
           //        fs->fragment_table[fragments_read + i].size, fs->fragment_table[fragments_read + i].unused);
        }
        fragments_read += entries_here;
    }
    free(fragment_index);

    if (fragments_read != super->fragments)
    {
        free(fs->fragment_table);
        fs->fragment_table = NULL;
        fprintf(stderr, "Incomplete fragment table: read %u/%u entries\n", fragments_read, super->fragments);
        return SQUASH_ERROR_INVALID_FILE;
    }
    //printf("Fragment table read: %u entries OK\n", fragments_read);
    return SQUASH_OK;
}

SQUASH_API squash_error_t squash_open(const char *filename, squash_fs_t **fs)
{
    if (!filename || !fs)
    {
        return SQUASH_ERROR_INVALID_FILE;
    }

    *fs = malloc(sizeof(squash_fs_t));
    if (!*fs)
    {
        return SQUASH_ERROR_MEMORY;
    }

    memset(*fs, 0, sizeof(squash_fs_t));

    (*fs)->file = fopen(filename, "rb");
    if (!(*fs)->file)
    {
        free(*fs);
        *fs = NULL;
        return SQUASH_ERROR_INVALID_FILE;
    }

    squash_error_t err = read_super_block((*fs)->file, &(*fs)->super);
    if (err != SQUASH_OK)
    {
        fclose((*fs)->file);
        free(*fs);
        *fs = NULL;
        return err;
    }

    err = init_decompressor(*fs);
    if (err != SQUASH_OK)
    {
        fclose((*fs)->file);
        free(*fs);
        *fs = NULL;
        return err;
    }

    err = read_inode_lookup_table(*fs);
    if (err != SQUASH_OK)
    {
        squash_decompressor_destroy((*fs)->decompressor);
        fclose((*fs)->file);
        free(*fs);
        *fs = NULL;
        return err;
    }

    err = read_fragment_table(*fs);
    if (err != SQUASH_OK)
    {
        free((*fs)->inode_lookup_table);
        squash_decompressor_destroy((*fs)->decompressor);
        fclose((*fs)->file);
        free(*fs);
        *fs = NULL;
        return err;
    }

    // Проверяем root_inode
    uint64_t root_inode_offset = (*fs)->super.root_inode;
    bool valid_root_inode = true;
    if (root_inode_offset >= (*fs)->super.bytes_used - (*fs)->super.inode_table_start)
    {
        valid_root_inode = false;
    }
    else if ((*fs)->inode_lookup_table && (*fs)->super.inodes > 0)
    {
        bool found = false;
        for (uint32_t i = 0; i < (*fs)->super.inodes; i++)
        {
            if ((*fs)->inode_lookup_table[i] == root_inode_offset)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            //printf("Warning: root_inode 0x%llX not found in lookup table\n", root_inode_offset);
            valid_root_inode = true;
        }
    }

    if (!valid_root_inode)
    {
        //printf("Warning: root_inode 0x%llX is invalid, searching for root directory\n",
          //     (*fs)->super.root_inode);
        err = find_root_inode(*fs);
        if (err != SQUASH_OK)
        {
            free((*fs)->fragment_table);
            free((*fs)->inode_lookup_table);
            squash_decompressor_destroy((*fs)->decompressor);
            fclose((*fs)->file);
            free(*fs);
            *fs = NULL;
            return err;
        }
    }

    (*fs)->filename = malloc(strlen(filename) + 1);
    if ((*fs)->filename)
    {
        strcpy((*fs)->filename, filename);
    }
    else
    {
        free((*fs)->fragment_table);
        free((*fs)->inode_lookup_table);
        squash_decompressor_destroy((*fs)->decompressor);
        fclose((*fs)->file);
        free(*fs);
        *fs = NULL;
        return SQUASH_ERROR_MEMORY;
    }

    return SQUASH_OK;
}

SQUASH_API void squash_close(squash_fs_t *fs)
{
    if (!fs)
        return;

    if (fs->file)
    {
        fclose(fs->file);
    }

    if (fs->fragment_table)
    {
        free(fs->fragment_table);
    }

    if (fs->inode_lookup_table)
    {
        free(fs->inode_lookup_table);
    }

    if (fs->id_table)
    {
        free(fs->id_table);
    }

    if (fs->filename)
    {
        free(fs->filename);
    }

    if (fs->decompressor)
    {
        squash_decompressor_destroy(fs->decompressor);
    }

    free(fs);
}

SQUASH_API squash_error_t squash_get_super(squash_fs_t *fs, squash_super_t *super)
{
    if (!fs || !super)
    {
        return SQUASH_ERROR_INVALID_FILE;
    }

    memcpy(super, &fs->super, sizeof(squash_super_t));
    return SQUASH_OK;
}