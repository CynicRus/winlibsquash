#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/libsquash/squash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/libsquash/squash.h"

SQUASH_API squash_error_t squash_read_file(squash_fs_t *fs, squash_reg_inode_t *inode,
                                           void *buffer, size_t offset, size_t size, size_t *bytes_read)
{
    if (!fs || !fs->file || !inode || !buffer || !bytes_read)
    {
        fprintf(stderr, "Invalid arguments: fs=%p, inode=%p, buffer=%p, bytes_read=%p\n",
                fs, inode, buffer, bytes_read);
        return SQUASH_ERROR_INVALID_FILE;
    }

    if (inode->base.inode_type != SQUASHFS_REG_TYPE && inode->base.inode_type != SQUASHFS_LREG_TYPE)
    {
        fprintf(stderr, "Invalid inode type: %d\n", inode->base.inode_type);
        return SQUASH_ERROR_NOT_FILE;
    }

    *bytes_read = 0;
    if (offset >= inode->file_size)
    {
        fprintf(stderr, "Offset %zu exceeds file size %llu\n", offset, inode->file_size);
        return SQUASH_OK;
    }

    size_t to_read = size;
    if (offset + size > inode->file_size)
    {
        to_read = inode->file_size - offset;
    }

    uint32_t block_size = fs->super.block_size;
    bool file_in_fragment_only = (inode->fragment != 0xFFFFFFFF &&
                                  inode->file_size <= block_size);

    uint32_t nblocks;
    if (file_in_fragment_only)
        nblocks = 0;
    else
    {
        nblocks = (inode->file_size + block_size - 1) / block_size;
        if (inode->fragment != 0xFFFFFFFF && inode->file_size % block_size != 0)
        {
            nblocks--;
        }
    }

    uint32_t start_block_idx = offset / block_size;
    size_t block_offset = offset % block_size;
    bool has_fragment = (inode->fragment != 0xFFFFFFFF);

    fprintf(stderr, "Reading file: size=%llu, block_size=%u, nblocks=%u, fragment=%u, offset=%zu, file_in_fragment_only=%d\n",
            inode->file_size, block_size, nblocks, inode->fragment, offset, file_in_fragment_only);

    fprintf(stderr, "block_list: %p\n", inode->block_list);
    if (inode->block_list)
    {
        fprintf(stderr, "Block_list entries: ");
        for (uint32_t i = 0; i < nblocks; i++)
        {
            fprintf(stderr, "0x%08X ", inode->block_list[i]);
        }
        fprintf(stderr, "\n");
    }

    if (!inode->block_list && start_block_idx < nblocks)
    {
        fprintf(stderr, "Invalid block_list for block_idx=%u\n", start_block_idx);
        return SQUASH_ERROR_IO;
    }

    size_t remaining = to_read;
    uint8_t *dest = (uint8_t *)buffer;
    uint64_t current_file_offset = inode->start_block;

    for (uint32_t i = 0; i < start_block_idx && i < nblocks; i++)
    {
        uint32_t block = inode->block_list[i];
        uint32_t compressed_size = block & ((1 << 24) - 1);
        current_file_offset += compressed_size;
        fprintf(stderr, "block[%u] offset contribution: (size) = 0x%x\n", i, compressed_size);
    }

    while (remaining > 0)
    {
        if (has_fragment && (file_in_fragment_only || start_block_idx == nblocks))
        {
            if (!fs->fragment_table)
            {
                fprintf(stderr, "Fragment table not loaded for fragment=%u\n", inode->fragment);
                return SQUASH_ERROR_IO;
            }

            if (inode->fragment >= fs->super.fragments)
            {
                fprintf(stderr, "Invalid fragment index %u (max=%u)\n", inode->fragment, fs->super.fragments);
                return SQUASH_ERROR_IO;
            }

            struct squashfs_fragment_entry *frag = &fs->fragment_table[inode->fragment];
            uint64_t start_block = frag->start_block;
            uint32_t size = frag->size;

            fprintf(stderr, "Fragment entry %u: start_block=0x%llx, size=%u\n", inode->fragment, start_block, size);

            if (start_block >= fs->super.bytes_used)
            {
                fprintf(stderr, "Fragment start_block 0x%llx exceeds bytes_used 0x%llx\n",
                        start_block, fs->super.bytes_used);
                return SQUASH_ERROR_INVALID_FILE;
            }

            bool is_compressed = !(size & (1 << 24));
            uint32_t actual_compressed_size = size & ((1 << 24) - 1);

            if (actual_compressed_size == 0 || actual_compressed_size > block_size)
            {
                fprintf(stderr, "Invalid fragment compressed size %u (max=%u)\n", actual_compressed_size, block_size);
                return SQUASH_ERROR_INVALID_FILE;
            }

            uint8_t *raw_block_data = NULL;
            size_t uncompressed_size = 0;
            squash_error_t err = squash_read_data_block(fs, start_block,
                                                        actual_compressed_size, is_compressed,
                                                        &raw_block_data, &uncompressed_size);
            if (err != SQUASH_OK)
            {
                fprintf(stderr, "Failed to read fragment block at 0x%llx\n", start_block);
                return err;
            }

            size_t fragment_data_offset = inode->offset + (file_in_fragment_only ? block_offset : 0);
            if (uncompressed_size < fragment_data_offset)
            {
                free(raw_block_data);
                fprintf(stderr, "Uncompressed fragment size %zu too small for offset %zu\n",
                        uncompressed_size, fragment_data_offset);
                return SQUASH_ERROR_INVALID_FILE;
            }

            size_t remaining_file_size = file_in_fragment_only ? inode->file_size : (inode->file_size % block_size);
            size_t copy_size = MIN(uncompressed_size - fragment_data_offset, MIN(remaining, remaining_file_size));
            if (copy_size > remaining_file_size)
            {
                free(raw_block_data);
                fprintf(stderr, "Copy size %zu exceeds remaining file size %zu\n",
                        copy_size, remaining_file_size);
                return SQUASH_ERROR_INVALID_FILE;
            }

            memcpy(dest, raw_block_data + fragment_data_offset, copy_size);
            free(raw_block_data);

            *bytes_read += copy_size;
            dest += copy_size;
            remaining -= copy_size;
            break;
        }
        else if (start_block_idx < nblocks)
        {
            size_t remaining_file_size = inode->file_size - (start_block_idx * block_size);
            size_t expected_uncompressed_size = MIN(block_size, remaining_file_size);

            uint32_t block = inode->block_list[start_block_idx];
            bool is_compressed = !(block & (1 << 24));
            uint32_t compressed_size = block & ((1 << 24) - 1);

            if (compressed_size == 0 && is_compressed)
            {
                // Это sparse block: просто заполняем нулями
                size_t to_zero = MIN(remaining, expected_uncompressed_size - block_offset);
                if (to_zero > 0)
                {
                    memset(dest, 0, to_zero);
                    *bytes_read += to_zero;
                    dest += to_zero;
                    remaining -= to_zero;
                    current_file_offset += compressed_size; // просто переходим к следующему
                    start_block_idx++;
                    block_offset = 0;
                    continue;
                }
            }
            else if (compressed_size == 0 || compressed_size > block_size)
            {
                // Это невалидный случай
                fprintf(stderr, "Invalid compressed_size %u for block %u\n", compressed_size, start_block_idx);
                return SQUASH_ERROR_INVALID_FILE;
            }

            fprintf(stderr, "Reading block: idx=%u, compressed=%d, compressed_size=%u, file_offset=0x%llx, expected_uncompressed_size=%zu\n",
                    start_block_idx, is_compressed, compressed_size, current_file_offset, expected_uncompressed_size);

            if (current_file_offset + compressed_size > fs->super.bytes_used)
            {
                fprintf(stderr, "Block offset 0x%llx + size %u exceeds bytes_used 0x%llx\n",
                        current_file_offset, compressed_size, fs->super.bytes_used);
                return SQUASH_ERROR_INVALID_FILE;
            }

            uint8_t *uncompressed_data = NULL;
            size_t uncompressed_size = 0;
            squash_error_t err = squash_read_data_block(fs, current_file_offset,
                                                        compressed_size, is_compressed,
                                                        &uncompressed_data, &uncompressed_size);
            if (err != SQUASH_OK)
            {
                fprintf(stderr, "Failed to read block at 0x%llx\n", current_file_offset);
                return err;
            }

            if (uncompressed_size < block_offset)
            {
                free(uncompressed_data);
                fprintf(stderr, "Uncompressed block size %zu too small for offset %zu\n",
                        uncompressed_size, block_offset);
                return SQUASH_ERROR_INVALID_FILE;
            }

            size_t copy_size = MIN(uncompressed_size - block_offset, MIN(remaining, expected_uncompressed_size));
            fprintf(stderr, "Copying %zu bytes from block %u\n", copy_size, start_block_idx);
            memcpy(dest, uncompressed_data + block_offset, copy_size);
            free(uncompressed_data);

            *bytes_read += copy_size;
            dest += copy_size;
            remaining -= copy_size;
            current_file_offset += compressed_size;
            start_block_idx++;
            block_offset = 0;
        }
        else
        {
            fprintf(stderr, "No more blocks or fragments to read: start_block_idx=%u, nblocks=%u, has_fragment=%d\n",
                    start_block_idx, nblocks, has_fragment);
            return SQUASH_ERROR_IO;
        }
    }

    if (remaining > 0)
    {
        fprintf(stderr, "Failed to read all requested bytes: remaining=%zu\n", remaining);
        return SQUASH_ERROR_IO;
    }

    return SQUASH_OK;
}

SQUASH_API squash_error_t squash_get_file_size(squash_reg_inode_t *inode, uint64_t *size)
{
    if (!inode || !size)
    {
        return SQUASH_ERROR_INVALID_FILE;
    }

    if (inode->base.inode_type != SQUASHFS_REG_TYPE && inode->base.inode_type != SQUASHFS_LREG_TYPE)
    {
        return SQUASH_ERROR_NOT_FILE;
    }

    *size = inode->file_size;
    return SQUASH_OK;
}