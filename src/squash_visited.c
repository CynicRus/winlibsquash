#include <stdlib.h>
#include <string.h>
#include "../include/libsquash/squash.h"

squash_error_t squash_visited_inodes_init(squash_visited_inodes_t *visited, size_t initial_capacity) {
    visited->inodes = malloc(initial_capacity * sizeof(squash_off_t));
    if (!visited->inodes) {
        return SQUASH_ERROR_MEMORY;
    }
    visited->count = 0;
    visited->capacity = initial_capacity;
    return SQUASH_OK;
}

void squash_visited_inodes_free(squash_visited_inodes_t *visited) {
    if (visited->inodes) {
        free(visited->inodes);
    }
    visited->inodes = NULL;
    visited->count = 0;
    visited->capacity = 0;
}

squash_error_t squash_visited_inodes_add(squash_visited_inodes_t *visited, squash_off_t inode_ref) {
    if (visited->count >= visited->capacity) {
        size_t new_capacity = visited->capacity * 2;
        squash_off_t *new_inodes = realloc(visited->inodes, new_capacity * sizeof(squash_off_t));
        if (!new_inodes) {
            return SQUASH_ERROR_MEMORY;
        }
        visited->inodes = new_inodes;
        visited->capacity = new_capacity;
    }
    visited->inodes[visited->count++] = inode_ref;
    return SQUASH_OK;
}

bool squash_visited_inodes_contains(squash_visited_inodes_t *visited, squash_off_t inode_ref) {
    for (size_t i = 0; i < visited->count; i++) {
        if (visited->inodes[i] == inode_ref) {
            return true;
        }
    }
    return false;
}