#include <stdio.h>
#include <stdlib.h>
#include "../include/libsquash/squash.h"

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <squashfs_image>\n", argv[0]);
        return 1;
    }

    squash_fs_t *fs;
    squash_error_t err = squash_open(argv[1], &fs);
    if (err != SQUASH_OK) {
        fprintf(stderr, "Failed to open SquashFS image: %s\n", squash_strerror(err));
        return 1;
    }

    squash_super_t super;
    err = squash_get_super(fs, &super);
    if (err != SQUASH_OK) {
        fprintf(stderr, "Failed to read superblock: %s\n", squash_strerror(err));
        squash_close(fs);
        return 1;
    }

    printf("SquashFS Image Info:\n");
    printf("Magic: 0x%08x\n", super.s_magic);
    printf("Inodes: %u\n", super.inodes);
    printf("Block Size: %u\n", super.block_size);
    printf("Compression: %s\n", squash_get_compression_name(super.compression));
    printf("Version: %u.%u\n", super.s_major, super.s_minor);
    printf("Bytes Used: %llu\n", super.bytes_used);

    squash_close(fs);
    return 0;
}