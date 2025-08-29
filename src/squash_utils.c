#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif
#include "../include/libsquash/squash.h"

squash_error_t read_fs_bytes(FILE *file, uint64_t start, size_t bytes, void *buffer)
{
    if (fseek(file, (long)start, SEEK_SET) != 0)
    {
        fprintf(stderr, "Failed to seek to offset 0x%llX: %s\n", start, strerror(errno));
        return SQUASH_ERROR_IO;
    }
    if (fread(buffer, 1, bytes, file) != bytes)
    {
        fprintf(stderr, "Failed to read %zu bytes at offset 0x%llX: %s\n", bytes, start, strerror(errno));
        return SQUASH_ERROR_IO;
    }
    return SQUASH_OK;
}

squash_error_t squash_read_metadata_block(squash_fs_t *fs, squash_off_t offset, uint8_t **uncompressed_data, size_t *uncompressed_size, size_t *compressed_size)
{
    if (offset >= fs->super.bytes_used)
    {
        fprintf(stderr, "Invalid metadata block offset: %llu exceeds bytes_used=%llu\n", offset, fs->super.bytes_used);
        return SQUASH_ERROR_INVALID_FILE;
    }
    if (fseek(fs->file, offset, SEEK_SET) != 0)
    {
        fprintf(stderr, "Error seeking to offset %llu: %s\n", offset, strerror(errno));
        return SQUASH_ERROR_IO;
    }
    uint16_t block_header;
    if (fread(&block_header, sizeof(uint16_t), 1, fs->file) != 1)
    {
        fprintf(stderr, "Error reading block header at offset %llu\n", offset);
        return SQUASH_ERROR_IO;
    }
    bool is_compressed = !(block_header & SQUASHFS_COMPRESSED_BIT_BLOCK);
    uint16_t block_size = block_header & SQUASHFS_COMPRESSED_SIZE_MASK;

    if (block_size == 0 || block_size > SQUASHFS_METADATA_SIZE || offset + 2 + block_size > fs->super.bytes_used)
    {
        fprintf(stderr, "Invalid block size %u at offset %llu, would exceed filesystem bounds\n", block_size, offset);
        return SQUASH_ERROR_INVALID_FILE;
    }

    uint8_t *compressed_data = malloc(block_size);
    if (!compressed_data)
    {
        return SQUASH_ERROR_MEMORY;
    }

    if (fread(compressed_data, 1, block_size, fs->file) != block_size)
    {
        free(compressed_data);
        fprintf(stderr, "Error reading block data at offset %llu\n", offset);
        return SQUASH_ERROR_IO;
    }

    *uncompressed_data = malloc(SQUASHFS_METADATA_SIZE);
    if (!*uncompressed_data)
    {
        free(compressed_data);
        return SQUASH_ERROR_MEMORY;
    }

    *uncompressed_size = SQUASHFS_METADATA_SIZE;
    squash_error_t err;

    if (is_compressed)
    {
        /*printf("Decompressing %u bytes, first 16: ", block_size);
        for (size_t i = 0; i < 16 && i < block_size; i++)
        {
            printf("%02x ", compressed_data[i]);
        }
        printf("\n");*/
        err = squash_decompress_block(fs->decompressor, compressed_data, block_size, *uncompressed_data, uncompressed_size);
        free(compressed_data);
        if (err != SQUASH_OK)
        {
            free(*uncompressed_data);
            *uncompressed_data = NULL;
            fprintf(stderr, "Decompression failed at offset %llu: %s\n", offset, squash_strerror(err));
            return err;
        }
        /*printf("Decompressed %zu bytes, first 16: ", *uncompressed_size);
        for (size_t i = 0; i < 16 && i < *uncompressed_size; i++)
        {
            printf("%02x ", (*uncompressed_data)[i]);
        }
        printf("\n");*/
    }
    else
    {
        memcpy(*uncompressed_data, compressed_data, block_size);
        *uncompressed_size = block_size;
        free(compressed_data);
    }

    *compressed_size = block_size;
    /*printf("Read metadata block: offset=0x%llx, compressed=%s, compressed_size=%u, uncompressed_size=%zu\n",
           offset, is_compressed ? "Yes" : "No", block_size, *uncompressed_size);*/
    return SQUASH_OK;
}

squash_error_t read_n_bytes_from_metablocks(squash_fs_t *fs, uint64_t start_offset, size_t offset_in_block,
                                            size_t n_bytes, uint8_t *out_buf, uint64_t *next_offset)
{
    size_t bytes_read = 0;
    uint64_t current_offset = start_offset;
    uint8_t *current_data = NULL;
    size_t current_size = 0;
    size_t pos = offset_in_block;

    // fprintf(stderr, "Reading %zu bytes from offset 0x%llx, pos=%zu\n", n_bytes, current_offset, pos);

    while (bytes_read < n_bytes)
    {
        if (!current_data || pos >= current_size)
        {
            free(current_data);
            current_data = NULL;

            // Читаем заголовок блока
            uint16_t block_header;
            if (read_fs_bytes(fs->file, current_offset, sizeof(block_header), &block_header) != SQUASH_OK)
            {
                fprintf(stderr, "Failed to read block header at 0x%llx: %s\n", current_offset, strerror(errno));
                free(current_data);
                return SQUASH_ERROR_IO;
            }
            block_header = squash_le16toh(block_header);

            bool is_compressed = SQUASHFS_COMPRESSED(block_header);
            uint16_t block_size = SQUASHFS_COMPRESSED_SIZE(block_header);
            // fprintf(stderr, "Block: compressed=%s, size=%u\n", is_compressed ? "Yes" : "No", block_size);

            if (block_size == 0 || block_size > SQUASHFS_METADATA_SIZE)
            {
                fprintf(stderr, "Invalid block size %u at offset 0x%llx\n", block_size, current_offset);
                free(current_data);
                return SQUASH_ERROR_IO;
            }

            // Читаем данные блока
            uint8_t *compressed_data = malloc(block_size);
            if (!compressed_data)
            {
                fprintf(stderr, "Memory allocation failed for block size %u\n", block_size);
                free(current_data);
                return SQUASH_ERROR_MEMORY;
            }

            if (read_fs_bytes(fs->file, current_offset + 2, block_size, compressed_data) != SQUASH_OK)
            {
                free(compressed_data);
                free(current_data);
                fprintf(stderr, "Failed to read block data at 0x%llx: %s\n", current_offset + 2, strerror(errno));
                return SQUASH_ERROR_IO;
            }

            // Декомпрессия (оставляем без изменений)
            current_size = SQUASHFS_METADATA_SIZE;
            current_data = malloc(current_size);
            if (!current_data)
            {
                free(compressed_data);
                fprintf(stderr, "Memory allocation failed for uncompressed data\n");
                return SQUASH_ERROR_MEMORY;
            }

            if (is_compressed)
            {
                squash_error_t err = squash_decompress_block(fs->decompressor, compressed_data, block_size,
                                                             current_data, &current_size);
                free(compressed_data);
                if (err != SQUASH_OK)
                {
                    free(current_data);
                    fprintf(stderr, "Failed to decompress block at 0x%llx: %s\n", current_offset, squash_strerror(err));
                    return err;
                }
            }
            else
            {
                memcpy(current_data, compressed_data, block_size);
                current_size = block_size;
                free(compressed_data);
            }
            // fprintf(stderr, "Uncompressed block size: %zu\n", current_size);

            // Дамп первых 64 байт блока для отладки
            if (current_offset == 0x251813a)
            {
                fprintf(stderr, "Block data at offset 0x%llx (first 64 bytes): ", current_offset);
                for (size_t i = 0; i < 64 && i < current_size; i++)
                {
                    fprintf(stderr, "%02x ", current_data[i]);
                }
                fprintf(stderr, "\n");
            }

            if (pos >= current_size)
            {
                fprintf(stderr, "Invalid pos=%zu, exceeds uncompressed_size=%zu\n", pos, current_size);
                free(current_data);
                return SQUASH_ERROR_INVALID_FILE;
            }

            current_offset += 2 + block_size;
            pos = 0;
        }

        size_t avail = current_size - pos;
        size_t to_copy = (n_bytes - bytes_read < avail) ? (n_bytes - bytes_read) : avail;
        if (to_copy == 0)
        {
            fprintf(stderr, "Invalid copy: pos=%zu, avail=%zu, uncompressed_size=%zu\n", pos, avail, current_size);
            free(current_data);
            return SQUASH_ERROR_INVALID_FILE;
        }

        memcpy(out_buf + bytes_read, current_data + pos, to_copy);
        bytes_read += to_copy;
        pos += to_copy;

        // fprintf(stderr, "Copied %zu bytes, total_read=%zu, pos=%zu\n", to_copy, bytes_read, pos);
    }

    // Дамп прочитанных данных
    /*fprintf(stderr, "Read data: ");
    for (size_t i = 0; i < n_bytes; i++) {
        fprintf(stderr, "%02x ", out_buf[i]);
    }
    fprintf(stderr, "\n");*/

    free(current_data);
    *next_offset = current_offset;
    return SQUASH_OK;
}

squash_error_t squash_read_data_block(squash_fs_t *fs, squash_off_t offset,
                                      uint32_t compressed_size, bool is_compressed,
                                      uint8_t **uncompressed_data, size_t *uncompressed_size)
{
    if (offset + compressed_size > fs->super.bytes_used)
    {
        fprintf(stderr, "Invalid data block offset: %llu + %u exceeds bytes_used=%llu\n",
                offset, compressed_size, fs->super.bytes_used);
        return SQUASH_ERROR_INVALID_FILE;
    }
    if (fseek(fs->file, offset, SEEK_SET) != 0)
    {
        fprintf(stderr, "Error seeking to offset %llu: %s\n", offset, strerror(errno));
        return SQUASH_ERROR_IO;
    }
    uint8_t *compressed_data = malloc(compressed_size);
    if (!compressed_data)
    {
        return SQUASH_ERROR_MEMORY;
    }
    if (fread(compressed_data, 1, compressed_size, fs->file) != compressed_size)
    {
        free(compressed_data);
        fprintf(stderr, "Error reading block data at offset %llu\n", offset);
        return SQUASH_ERROR_IO;
    }

    *uncompressed_data = malloc(fs->super.block_size);
    if (!*uncompressed_data)
    {
        free(compressed_data);
        return SQUASH_ERROR_MEMORY;
    }
    *uncompressed_size = fs->super.block_size;

    squash_error_t err;
    if (is_compressed)
    {
        err = squash_decompress_block(fs->decompressor, compressed_data, compressed_size,
                                      *uncompressed_data, uncompressed_size);
        free(compressed_data);
        if (err != SQUASH_OK)
        {
            free(*uncompressed_data);
            *uncompressed_data = NULL;
            fprintf(stderr, "Decompression failed at offset %llu: %s\n", offset, squash_strerror(err));
            return err;
        }
    }
    else
    {
        memcpy(*uncompressed_data, compressed_data, compressed_size);
        *uncompressed_size = compressed_size;
        free(compressed_data);
    }
    /*printf("Read data block: offset=0x%llx, compressed=%s, compressed_size=%u, uncompressed_size=%zu\n",
           offset, is_compressed ? "Yes" : "No", compressed_size, *uncompressed_size);*/
    return SQUASH_OK;
}

SQUASH_API squash_error_t squash_extract_file_by_inode(squash_fs_t *fs, squash_off_t inode_ref, const char *output_path)
{
    if (!fs || !output_path)
    {
        return SQUASH_ERROR_INVALID_FILE;
    }

    void *inode;
    squash_error_t err = squash_read_inode(fs, inode_ref, &inode);
    if (err != SQUASH_OK)
    {
        return err;
    }

    if (!squash_is_file(inode))
    {
        squash_free_inode(inode);
        return SQUASH_ERROR_NOT_FILE;
    }

    squash_reg_inode_t *reg_inode = (squash_reg_inode_t *)inode;
    uint64_t file_size;
    err = squash_get_file_size(reg_inode, &file_size);
    if (err != SQUASH_OK)
    {
        squash_free_inode(inode);
        fprintf(stderr, "Failed to get file size for inode_ref 0x%llx: %s\n", inode_ref, squash_strerror(err));
        return err;
    }
    fprintf(stderr, "File size: %llu bytes, inode_ref=0x%llx\n", (unsigned long long)file_size, inode_ref);

    // Проверяем и создаём родительскую директорию
    char *output_dir = strdup(output_path);
    if (!output_dir)
    {
        squash_free_inode(inode);
        fprintf(stderr, "Memory allocation failed for output_dir\n");
        return SQUASH_ERROR_MEMORY;
    }
    char *last_slash = strrchr(output_dir, '/');
#ifdef _WIN32
    if (!last_slash)
        last_slash = strrchr(output_dir, '\\');
#endif
    if (last_slash)
    {
        *last_slash = '\0';
#ifdef _WIN32
        if (_mkdir(output_dir) != 0 && errno != EEXIST)
        {
#else
        if (mkdir(output_dir, 0755) != 0 && errno != EEXIST)
        {
#endif
            fprintf(stderr, "Failed to create parent directory %s: %s\n", output_dir, strerror(errno));
            free(output_dir);
            squash_free_inode(inode);
            return SQUASH_ERROR_IO;
        }
    }
    free(output_dir);

    FILE *out_file = fopen(output_path, "wb");
    if (!out_file)
    {
        squash_free_inode(inode);
        fprintf(stderr, "Failed to open output file %s: %s\n", output_path, strerror(errno));
        return SQUASH_ERROR_IO;
    }

    // Используем буфер размером с блок SquashFS
    uint32_t block_size = fs->super.block_size;
    uint8_t *buffer = malloc(block_size);
    if (!buffer)
    {
        fclose(out_file);
        squash_free_inode(inode);
        fprintf(stderr, "Memory allocation failed for buffer (block_size=%u)\n", block_size);
        return SQUASH_ERROR_MEMORY;
    }

    uint64_t offset = 0;
    while (offset < file_size)
    {
        size_t bytes_to_read = (size_t)MIN(block_size, file_size - offset);
        size_t bytes_read;

       // fprintf(stderr, "Reading block at offset %llu, bytes: %zu\n", (unsigned long long)offset, bytes_to_read);
        err = squash_read_file(fs, reg_inode, buffer, offset, bytes_to_read, &bytes_read);
        if (err != SQUASH_OK)
        {
            fprintf(stderr, "Failed to read %zu bytes at offset %llu for %llu: %s\n", bytes_to_read, (unsigned long long)offset, inode_ref, squash_strerror(err));
            free(buffer);
            fclose(out_file);
            squash_free_inode(inode);
            return err;
        }

        if (bytes_read != bytes_to_read)
        {
            fprintf(stderr, "Read %zu bytes, expected %zu at offset %llu for %llu\n", bytes_read, bytes_to_read, (unsigned long long)offset, inode_ref);
            free(buffer);
            fclose(out_file);
            squash_free_inode(inode);
            return SQUASH_ERROR_IO;
        }

        if (fwrite(buffer, 1, bytes_read, out_file) != bytes_read)
        {
            fprintf(stderr, "Failed to write %zu bytes to %s: %s\n", bytes_read, output_path, strerror(errno));
            free(buffer);
            fclose(out_file);
            squash_free_inode(inode);
            return SQUASH_ERROR_IO;
        }

        offset += bytes_read;
    }

    free(buffer);
    fclose(out_file);

    squash_free_inode(inode);
    return SQUASH_OK;
}

SQUASH_API squash_error_t squash_extract_file(squash_fs_t *fs, const char *path, const char *output_path)
{
    if (!fs || !path || !output_path)
    {
        fprintf(stderr, "Invalid arguments: fs=%p, path=%s, output_path=%s\n", fs, path ? path : "NULL", output_path ? output_path : "NULL");
        return SQUASH_ERROR_INVALID_FILE;
    }

    fprintf(stderr, "Extracting file: %s -> %s\n", path, output_path);

    squash_off_t inode_ref;
    squash_error_t err = squash_lookup_path(fs, path, &inode_ref);
    if (err != SQUASH_OK)
    {
        fprintf(stderr, "Failed to lookup path %s: %s\n", path, squash_strerror(err));
        return err;
    }

    void *inode;
    err = squash_read_inode(fs, inode_ref, &inode);
    if (err != SQUASH_OK)
    {
        fprintf(stderr, "Failed to read inode for %s: %s\n", path, squash_strerror(err));
        return err;
    }

    if (!squash_is_file(inode))
    {
        squash_free_inode(inode);
        fprintf(stderr, "%s is not a file (type=%d)\n", path, ((squash_reg_inode_t *)inode)->base.inode_type);
        return SQUASH_ERROR_NOT_FILE;
    }

    squash_reg_inode_t *reg_inode = (squash_reg_inode_t *)inode;
    uint64_t file_size;
    err = squash_get_file_size(reg_inode, &file_size);
    if (err != SQUASH_OK)
    {
        squash_free_inode(inode);
        fprintf(stderr, "Failed to get file size for %s: %s\n", path, squash_strerror(err));
        return err;
    }
    fprintf(stderr, "File size: %llu bytes, inode_ref=0x%llx\n", (unsigned long long)file_size, inode_ref);

    // Проверяем и создаём родительскую директорию
    char *output_dir = strdup(output_path);
    if (!output_dir)
    {
        squash_free_inode(inode);
        fprintf(stderr, "Memory allocation failed for output_dir\n");
        return SQUASH_ERROR_MEMORY;
    }
    char *last_slash = strrchr(output_dir, '/');
#ifdef _WIN32
    if (!last_slash)
        last_slash = strrchr(output_dir, '\\');
#endif
    if (last_slash)
    {
        *last_slash = '\0';
#ifdef _WIN32
        if (_mkdir(output_dir) != 0 && errno != EEXIST)
        {
#else
        if (mkdir(output_dir, 0755) != 0 && errno != EEXIST)
        {
#endif
            fprintf(stderr, "Failed to create parent directory %s: %s\n", output_dir, strerror(errno));
            free(output_dir);
            squash_free_inode(inode);
            return SQUASH_ERROR_IO;
        }
    }
    free(output_dir);

    FILE *out_file = fopen(output_path, "wb");
    if (!out_file)
    {
        squash_free_inode(inode);
        fprintf(stderr, "Failed to open output file %s: %s\n", output_path, strerror(errno));
        return SQUASH_ERROR_IO;
    }

    // Используем буфер размером с блок SquashFS
    uint32_t block_size = fs->super.block_size;
    uint8_t *buffer = malloc(block_size);
    if (!buffer)
    {
        fclose(out_file);
        squash_free_inode(inode);
        fprintf(stderr, "Memory allocation failed for buffer (block_size=%u)\n", block_size);
        return SQUASH_ERROR_MEMORY;
    }

    uint64_t offset = 0;
    while (offset < file_size)
    {
        size_t bytes_to_read = (size_t)MIN(block_size, file_size - offset);
        size_t bytes_read;

        fprintf(stderr, "Reading block at offset %llu, bytes: %zu\n", (unsigned long long)offset, bytes_to_read);
        err = squash_read_file(fs, reg_inode, buffer, offset, bytes_to_read, &bytes_read);
        if (err != SQUASH_OK)
        {
            fprintf(stderr, "Failed to read %zu bytes at offset %llu for %s: %s\n", bytes_to_read, (unsigned long long)offset, path, squash_strerror(err));
            free(buffer);
            fclose(out_file);
            squash_free_inode(inode);
            return err;
        }

        if (bytes_read != bytes_to_read)
        {
            fprintf(stderr, "Read %zu bytes, expected %zu at offset %llu for %s\n", bytes_read, bytes_to_read, (unsigned long long)offset, path);
            free(buffer);
            fclose(out_file);
            squash_free_inode(inode);
            return SQUASH_ERROR_IO;
        }

        if (fwrite(buffer, 1, bytes_read, out_file) != bytes_read)
        {
            fprintf(stderr, "Failed to write %zu bytes to %s: %s\n", bytes_read, output_path, strerror(errno));
            free(buffer);
            fclose(out_file);
            squash_free_inode(inode);
            return SQUASH_ERROR_IO;
        }

        offset += bytes_read;
    }

    free(buffer);
    fclose(out_file);
    squash_free_inode(inode);
    fprintf(stderr, "Successfully extracted %s\n", output_path);
    return SQUASH_OK;
}

static squash_error_t squash_extract_directory_recursive(
    squash_fs_t *fs,
    squash_off_t inode_ref,
    const char *output_dir,
    squash_visited_inodes_t *visited)
{
    // Проверяем циклы
    if (squash_visited_inodes_contains(visited, inode_ref))
    {
        //printf("Cycle detected: skipping inode_ref 0x%llx\n", inode_ref);
        return SQUASH_OK; // Не ошибка, просто пропускаем
    }

    // Добавляем в visited
    squash_error_t err = squash_visited_inodes_add(visited, inode_ref);
    if (err != SQUASH_OK)
    {
        return err;
    }

    // Читаем inode ОДИН раз
    void *inode;
    err = squash_read_inode(fs, inode_ref, &inode);
    if (err != SQUASH_OK)
    {
        return err;
    }

    if (!squash_is_directory(inode))
    {
        squash_free_inode(inode);
        return SQUASH_ERROR_NOT_DIRECTORY;
    }

    // Создаем выходную директорию
    if (mkdir(output_dir, 0755) != 0 && errno != EEXIST)
    {
        squash_free_inode(inode);
        return SQUASH_ERROR_IO;
    }

    squash_dir_inode_t *dir_inode = (squash_dir_inode_t *)inode;
    squash_dir_iterator_t *iterator;
    err = squash_opendir(fs, dir_inode, &iterator);
    if (err != SQUASH_OK)
    {
        squash_free_inode(inode);
        return err;
    }

    squash_dir_entry_t *entry;
    while (squash_readdir(iterator, &entry) == SQUASH_OK && entry)
    {
        //printf("Processing entry: name=%s, inode_ref=0x%llx\n", entry->name, entry->inode_ref);

        char *new_output_path = malloc(strlen(output_dir) + strlen(entry->name) + 2);
        if (!new_output_path)
        {
            squash_free_dir_entry(entry);
            squash_closedir(iterator);
            squash_free_inode(inode);
            return SQUASH_ERROR_MEMORY;
        }
        sprintf(new_output_path, "%s/%s", output_dir, entry->name);

        // Читаем inode записи
        void *entry_inode;
        err = squash_read_inode(fs, entry->inode_ref, &entry_inode);
        if (err != SQUASH_OK)
        {
            free(new_output_path);
            squash_free_dir_entry(entry);
            squash_closedir(iterator);
            squash_free_inode(inode);
            return err;
        }

        if (squash_is_directory(entry_inode))
        {
            // РЕКУРСИЯ БЕЗ LOOKUP! Передаем inode_ref напрямую
            err = squash_extract_directory_recursive(fs, entry->inode_ref, new_output_path, visited);
        }
        else if (squash_is_file(entry_inode))
        {
            err = squash_extract_file_by_inode(fs, entry->inode_ref, new_output_path);
        }

        free(new_output_path);
        squash_free_inode(entry_inode);
        squash_free_dir_entry(entry);

        if (err != SQUASH_OK)
        {
            squash_closedir(iterator);
            squash_free_inode(inode);
            return err;
        }
    }

    squash_closedir(iterator);
    squash_free_inode(inode);
    return SQUASH_OK;
}

// Публичная функция
SQUASH_API squash_error_t squash_extract_directory(squash_fs_t *fs, const char *path, const char *output_dir)
{
    if (!fs || !path || !output_dir)
    {
        return SQUASH_ERROR_INVALID_FILE;
    }

    // Lookup делаем ТОЛЬКО ОДИН раз в начале
    squash_off_t inode_ref;
    squash_error_t err = squash_lookup_path(fs, path, &inode_ref);
    if (err != SQUASH_OK)
    {
        return err;
    }

    // Создаем visited для всей операции
    squash_visited_inodes_t visited;
    err = squash_visited_inodes_init(&visited, 16);
    if (err != SQUASH_OK)
    {
        return err;
    }

    // Вызываем рекурсивную функцию
    err = squash_extract_directory_recursive(fs, inode_ref, output_dir, &visited);

    squash_visited_inodes_free(&visited);
    return err;
}

SQUASH_API squash_error_t squash_list_directory(squash_fs_t *fs, const char *path)
{
    if (!fs || !path)
    {
        return SQUASH_ERROR_INVALID_FILE;
    }

    squash_off_t inode_ref;
    squash_error_t err = squash_lookup_path(fs, path, &inode_ref);
    if (err != SQUASH_OK)
    {
        return err;
    }

    void *inode;
    err = squash_read_inode(fs, inode_ref, &inode);
    if (err != SQUASH_OK)
    {
        return err;
    }

    if (!squash_is_directory(inode))
    {
        squash_free_inode(inode);
        return SQUASH_ERROR_NOT_DIRECTORY;
    }

    squash_dir_inode_t *dir_inode = (squash_dir_inode_t *)inode;
    squash_dir_iterator_t *iterator;
    err = squash_opendir(fs, dir_inode, &iterator);
    if (err != SQUASH_OK)
    {
        squash_free_inode(inode);
        return err;
    }

    squash_dir_entry_t *entry;
    while (squash_readdir(iterator, &entry) == SQUASH_OK && entry)
    {
       // printf("%s (inode_ref=0x%llx)\n", entry->name, entry->inode_ref);
        squash_free_dir_entry(entry);
    }

    squash_closedir(iterator);
    squash_free_inode(inode);
    return SQUASH_OK;
}

SQUASH_API const char *squash_get_compression_name(uint16_t compression)
{
    switch (compression)
    {
    case 1:
        return "GZIP";
    case 2:
        return "LZMA";
    case 3:
        return "LZO";
    case 4:
        return "XZ";
    case 5:
        return "LZ4";
    default:
        return "Unknown";
    }
}

SQUASH_API bool squash_is_file(void *inode)
{
    if (!inode)
        return false;
    uint16_t inode_type = *(uint16_t *)inode;
    return inode_type == SQUASHFS_REG_TYPE || inode_type == SQUASHFS_LREG_TYPE;
}

SQUASH_API bool squash_is_directory(void *inode)
{
    if (!inode)
        return false;
    uint16_t inode_type = *(uint16_t *)inode;
    return inode_type == SQUASHFS_DIR_TYPE || inode_type == SQUASHFS_LDIR_TYPE;
}

SQUASH_API bool squash_is_symlink(void *inode)
{
    if (!inode)
        return false;
    uint16_t inode_type = *(uint16_t *)inode;
    return inode_type == SQUASHFS_SYMLINK_TYPE || inode_type == SQUASHFS_LSYMLINK_TYPE;
}