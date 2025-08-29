#include <stdio.h>
#include <stdlib.h>
#include "../include/libsquash/squash.h"

int main(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <squashfs_image> <path> <output_path>\n", argv[0]);
        return 1;
    }

    squash_fs_t *fs;
    squash_error_t err = squash_open(argv[1], &fs);
    if (err != SQUASH_OK) {
        fprintf(stderr, "Failed to open SquashFS image: %s\n", squash_strerror(err));
        return 1;
    }

    squash_off_t inode_ref;
    err = squash_lookup_path(fs, argv[2], &inode_ref);
    if (err != SQUASH_OK) {
        fprintf(stderr, "Failed to find path: %s\n", squash_strerror(err));
        squash_close(fs);
        return 1;
    }

    void *inode;
    err = squash_read_inode(fs, inode_ref, &inode);
    if (err != SQUASH_OK) {
        fprintf(stderr, "Failed to read inode: %s\n", squash_strerror(err));
        squash_close(fs);
        return 1;
    }

    if (squash_is_directory(inode)) {
        err = squash_extract_directory(fs, argv[2], argv[3]);
    } else if (squash_is_file(inode)) {
        err = squash_extract_file(fs, argv[2], argv[3]);
    } else {
        err = SQUASH_ERROR_NOT_FILE;
    }

    if (err != SQUASH_OK) {
        fprintf(stderr, "Failed to extract: %s\n", squash_strerror(err));
    }

    squash_free_inode(inode);
    squash_close(fs);
    return err == SQUASH_OK ? 0 : 1;
}