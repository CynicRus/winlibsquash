#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "../include/libsquash/squash.h"

static squash_error_t add_dir_entry(dir_entries_list_t *list, squash_dir_entry_t *entry)
{
    if (list->count >= list->capacity)
    {
        size_t new_capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        squash_dir_entry_t **new_entries = realloc(list->entries, new_capacity * sizeof(squash_dir_entry_t *));
        if (!new_entries)
        {
            return SQUASH_ERROR_MEMORY;
        }
        list->entries = new_entries;
        list->capacity = new_capacity;
    }
    list->entries[list->count++] = entry;
    return SQUASH_OK;
}


static squash_error_t squash_read_bytes(squash_fs_t *fs, uint64_t *current_offset,
                                        uint8_t **uncompressed_data, size_t *uncompressed_size,
                                        size_t *pos, size_t size, uint8_t *out_buf,
                                        size_t *left_in_dir, bool *first_block)
{
    size_t filled = 0;

    printf("Reading %zu bytes at offset 0x%llx, pos=%zu, left_in_dir=%zu\n",
           size, *current_offset, *pos, *left_in_dir);

    while (filled < size)
    {
        if (!*uncompressed_data || *pos >= *uncompressed_size)
        {
            size_t compressed_size = 0;
            free(*uncompressed_data);
            *uncompressed_data = NULL;
            printf("Loading new block at offset 0x%llx\n", *current_offset);
            squash_error_t err = squash_read_metadata_block(fs, *current_offset, uncompressed_data, uncompressed_size, &compressed_size);
            if (err != SQUASH_OK)
            {
                printf("Failed to read metadata block: err=%d\n", err);
                return err;
            }
            *current_offset += 2 + compressed_size;
            *pos = 0;
            *first_block = false;
            printf("New block loaded, uncompressed_size=%zu, pos=%zu\n", *uncompressed_size, *pos);
        }

        size_t avail = *uncompressed_size - *pos;
        size_t to_copy = (size - filled < avail) ? (size - filled) : avail;

        if (to_copy == 0 || *pos + to_copy > *uncompressed_size)
        {
            printf("Invalid copy: pos=%zu, to_copy=%zu, uncompressed_size=%zu\n", *pos, to_copy, *uncompressed_size);
            free(*uncompressed_data);
            *uncompressed_data = NULL;
            return SQUASH_ERROR_INVALID_FILE;
        }

        if (*left_in_dir < to_copy)
        {
            printf("Not enough data in directory: left_in_dir=%zu, need=%zu\n", *left_in_dir, to_copy);
            free(*uncompressed_data);
            *uncompressed_data = NULL;
            return SQUASH_ERROR_INVALID_FILE;
        }

        memcpy(out_buf + filled, *uncompressed_data + *pos, to_copy);
        filled += to_copy;
        *pos += to_copy;
        *left_in_dir -= to_copy;

        printf("Copied %zu bytes, filled=%zu, pos=%zu, left_in_dir=%zu\n",
               to_copy, filled, *pos, *left_in_dir);
        printf("Copied data: ");
        for (size_t i = 0; i < to_copy; i++)
        {
            printf("%02x ", out_buf[filled - to_copy + i]);
        }
        printf("\n");
    }

    return SQUASH_OK;
}

SQUASH_API squash_error_t squash_opendir(squash_fs_t *fs, squash_dir_inode_t *dir_inode, squash_dir_iterator_t **iterator)
{
    if (!fs || !dir_inode || !iterator)
    {
        printf("Invalid parameters: fs=%p, dir_inode=%p, iterator=%p\n",
               (void *)fs, (void *)dir_inode, (void *)iterator);
        return SQUASH_ERROR_INVALID_FILE;
    }

    uint64_t base_offset = fs->super.directory_table_start + dir_inode->start_block;
    uint16_t dir_offset = dir_inode->offset;

    printf("Directory inode: start_block=%u, offset=%u, size=%u\n",
           dir_inode->start_block, dir_offset, dir_inode->file_size);
    printf("Directory table start: 0x%llx\n", fs->super.directory_table_start);
    printf("Reading directory at: 0x%llx\n", base_offset);

    if (base_offset >= fs->super.bytes_used)
    {
        printf("Invalid directory block offset: 0x%llu >= bytes_used=0x%llu\n",
               base_offset, fs->super.bytes_used);
        return SQUASH_ERROR_INVALID_FILE;
    }

    dir_entries_list_t entries_list = {0};
    uint8_t *uncompressed_data = NULL;
    size_t uncompressed_size = 0;
    uint64_t current_offset = base_offset;
    size_t left_in_dir = dir_inode->file_size;
    size_t pos = dir_offset;
    bool first_block = true;
    squash_error_t err = SQUASH_OK;

    while (left_in_dir >= 12)
    {
        if (!uncompressed_data || pos >= uncompressed_size)
        {
            size_t compressed_size = 0;
            free(uncompressed_data);
            uncompressed_data = NULL;
            printf("Loading new block at offset 0x%llx\n", current_offset);
            err = squash_read_metadata_block(fs, current_offset, &uncompressed_data, &uncompressed_size, &compressed_size);
            if (err != SQUASH_OK)
            {
                for (size_t i = 0; i < entries_list.count; i++)
                {
                    squash_free_dir_entry(entries_list.entries[i]);
                }
                free(entries_list.entries);
                return err;
            }
            current_offset += 2 + compressed_size;
            pos = first_block ? dir_offset : 0;
            first_block = false;
            printf("Block data at pos=%zu: ", pos);
            for (size_t i = pos; i < pos + 16 && i < uncompressed_size; i++)
            {
                printf("%02x ", uncompressed_data[i]);
            }
            printf("\n");
        }

        if (left_in_dir < 12)
        {
            printf("Reached end of directory data: left_in_dir=%zu\n", left_in_dir);
            break; // Выходим из цикла, так как больше нет групп для обработки
        }

        uint8_t header_buf[12];
        err = squash_read_bytes(fs, &current_offset, &uncompressed_data, &uncompressed_size,
                                &pos, 12, header_buf, &left_in_dir, &first_block);
        if (err != SQUASH_OK)
        {
            printf("Failed to read group header: %s\n", squash_strerror(err));
            free(uncompressed_data);
            for (size_t i = 0; i < entries_list.count; i++)
            {
                squash_free_dir_entry(entries_list.entries[i]);
            }
            free(entries_list.entries);
            return err;
        }

        printf("Group header: ");
        for (size_t i = 0; i < 12; i++)
        {
            printf("%02x ", header_buf[i]);
        }
        printf("\n");

        uint32_t count = *(uint32_t *)(header_buf) + 1; // count хранит count-1
        uint32_t start_block = *(uint32_t *)(header_buf + 4);
        uint32_t header_inode_number = *(uint32_t *)(header_buf + 8);

        printf("Parsed group header: count=%u, start_block=%u, header_inode_number=%u\n",
               count, start_block, header_inode_number);

        // Парсим заголовки один за другим
        for (uint32_t i = 0; i < count; i++)
        {
            if (left_in_dir < 8)
                break;
            // Читаем заголовок записи (8 байт)
            uint8_t entry_header[8];
            err = squash_read_bytes(fs, &current_offset, &uncompressed_data, &uncompressed_size,
                                    &pos, 8, entry_header, &left_in_dir, &first_block);
            if (err != SQUASH_OK)
            {
                printf("Failed to read entry header %u: %s\n", i, squash_strerror(err));
                free(uncompressed_data);
                for (size_t j = 0; j < entries_list.count; j++)
                {
                    squash_free_dir_entry(entries_list.entries[j]);
                }
                free(entries_list.entries);
                return err;
            }

            printf("Entry %u header: ", i);
            for (size_t j = 0; j < 8; j++)
            {
                printf("%02x ", entry_header[j]);
            }
            printf("\n");

            int16_t offset_field = (int16_t)GET_LE16(entry_header);
            int16_t inode_offset = (int16_t)GET_LE16(entry_header + 2);
            uint16_t type = (uint16_t)GET_LE16(entry_header + 4);
            uint16_t name_size = (uint16_t)GET_LE16(entry_header + 6) + 1;

            printf("Parsed entry %u: offset_field=%d, inode_offset=%d, type=%u, name_size=%u\n",
                   i, offset_field, inode_offset, type, name_size);

            // Проверяем валидность типа
            if (type < SQUASHFS_DIR_TYPE || type > SQUASHFS_CHRDEV_TYPE)
            {
                printf("Invalid entry type: type=%u\n", type);
                free(uncompressed_data);
                for (size_t j = 0; j < entries_list.count; j++)
                {
                    squash_free_dir_entry(entries_list.entries[j]);
                }
                free(entries_list.entries);
                return SQUASH_ERROR_INVALID_FILE;
            }

            // Проверяем валидность размера имени
            if (name_size == 0 || name_size > 255)
            {
                printf("Invalid entry name size: name_size=%u\n", name_size);
                free(uncompressed_data);
                for (size_t j = 0; j < entries_list.count; j++)
                {
                    squash_free_dir_entry(entries_list.entries[j]);
                }
                free(entries_list.entries);
                return SQUASH_ERROR_INVALID_FILE;
            }

            if (left_in_dir < name_size + 1)
            {
                break;
            }

            // Читаем имя файла
            if (left_in_dir < name_size + 1) // +1 for null terminator in SquashFS
            {
                printf("Not enough data for entry name: left_in_dir=%zu, name_size=%u\n", left_in_dir, name_size + 1);
                free(uncompressed_data);
                for (size_t j = 0; j < entries_list.count; j++)
                {
                    squash_free_dir_entry(entries_list.entries[j]);
                }
                free(entries_list.entries);
                return SQUASH_ERROR_INVALID_FILE;
            }

            char *name = malloc(name_size + 1); // только +1 для завершающего нуля
            if (!name)
            {
                printf("Memory allocation failed for name\n");
                free(uncompressed_data);
                for (size_t j = 0; j < entries_list.count; j++)
                {
                    squash_free_dir_entry(entries_list.entries[j]);
                }
                free(entries_list.entries);
                return SQUASH_ERROR_MEMORY;
            }

            // ИСПРАВЛЕНИЕ: читаем только name_size байт
            err = squash_read_bytes(fs, &current_offset, &uncompressed_data, &uncompressed_size,
                                    &pos, name_size, (uint8_t *)name, &left_in_dir, &first_block);
            if (err != SQUASH_OK)
            {
                printf("Failed to read entry name: %s\n", squash_strerror(err));
                free(name);
                free(uncompressed_data);
                for (size_t j = 0; j < entries_list.count; j++)
                {
                    squash_free_dir_entry(entries_list.entries[j]);
                }
                free(entries_list.entries);
                return err;
            }
            name[name_size] = '\0';

            printf("Read name: '%s' (length=%zu, name_size=%u)\n", name, strlen(name), name_size);

            // Пропускаем . и ..
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            {
                free(name);
                continue;
            }

            // Вычисляем параметры записи
            uint32_t inode_number = header_inode_number + inode_offset;
            uint64_t entry_inode_ref = ((uint64_t)start_block << 16) | ((uint16_t)offset_field);

            printf("Creating entry: name='%s', inode_ref=0x%llx, inode_number=%u, type=%u\n",
                   name, entry_inode_ref, inode_number, type);

            // Создаем запись
            squash_dir_entry_t *entry = malloc(sizeof(squash_dir_entry_t));
            if (!entry)
            {
                printf("Memory allocation failed for entry\n");
                free(name);
                free(uncompressed_data);
                for (size_t j = 0; j < entries_list.count; j++)
                {
                    squash_free_dir_entry(entries_list.entries[j]);
                }
                free(entries_list.entries);
                return SQUASH_ERROR_MEMORY;
            }

            entry->inode_ref = entry_inode_ref;
            entry->inode_number = inode_number;
            entry->type = type;
            entry->size = strlen(name) + 1;
            entry->name = name;

            err = add_dir_entry(&entries_list, entry);
            if (err != SQUASH_OK)
            {
                printf("Failed to add entry %u: %s\n", i, squash_strerror(err));
                squash_free_dir_entry(entry);
                free(uncompressed_data);
                for (size_t j = 0; j < entries_list.count; j++)
                {
                    squash_free_dir_entry(entries_list.entries[j]);
                }
                free(entries_list.entries);
                return err;
            }
        }
    }
    free(uncompressed_data);

    *iterator = malloc(sizeof(squash_dir_iterator_t));
    if (!*iterator)
    {
        for (size_t i = 0; i < entries_list.count; i++)
        {
            squash_free_dir_entry(entries_list.entries[i]);
        }
        free(entries_list.entries);
        return SQUASH_ERROR_MEMORY;
    }

    (*iterator)->fs = fs;
    (*iterator)->dir_inode = dir_inode;
    (*iterator)->uncompressed_data = (uint8_t *)entries_list.entries;
    (*iterator)->uncompressed_size = entries_list.count;
    (*iterator)->current_entry_index = 0;
    (*iterator)->finished = false;

    printf("Successfully loaded %zu directory entries\n", entries_list.count);
    return SQUASH_OK;
}

SQUASH_API squash_error_t squash_readdir(squash_dir_iterator_t *iterator, squash_dir_entry_t **entry)
{
    if (!iterator || !entry || iterator->finished)
    {
        if (entry)
            *entry = NULL;
        return SQUASH_OK;
    }

    squash_dir_entry_t **entries = (squash_dir_entry_t **)iterator->uncompressed_data;
    size_t total_entries = iterator->uncompressed_size;

    if (iterator->current_entry_index >= total_entries)
    {
        iterator->finished = true;
        *entry = NULL;
        return SQUASH_OK;
    }

    squash_dir_entry_t *src = entries[iterator->current_entry_index];
    *entry = malloc(sizeof(squash_dir_entry_t));
    if (!*entry)
    {
        return SQUASH_ERROR_MEMORY;
    }

    (*entry)->inode_ref = src->inode_ref;
    (*entry)->inode_number = src->inode_number;
    (*entry)->type = src->type;
    (*entry)->size = src->size;
    (*entry)->name = malloc(src->size);
    if (!(*entry)->name)
    {
        free(*entry);
        *entry = NULL;
        return SQUASH_ERROR_MEMORY;
    }
    memcpy((*entry)->name, src->name, src->size);

    printf("Returning directory entry: name=%s, inode_ref=0x%llx, inode_number=%u, type=%u\n",
           (*entry)->name, (*entry)->inode_ref, (*entry)->inode_number, (*entry)->type);

    iterator->current_entry_index++;

    return SQUASH_OK;
}

SQUASH_API void squash_closedir(squash_dir_iterator_t *iterator)
{
    if (iterator)
    {
        if (iterator->uncompressed_data)
        {
            squash_dir_entry_t **entries = (squash_dir_entry_t **)iterator->uncompressed_data;
            for (size_t i = 0; i < iterator->uncompressed_size; i++)
            {
                squash_free_dir_entry(entries[i]);
            }
            free(entries);
        }
        free(iterator);
    }
}

SQUASH_API void squash_free_dir_entry(squash_dir_entry_t *entry)
{
    if (entry)
    {
        if (entry->name)
        {
            free(entry->name);
        }
        free(entry);
    }
}