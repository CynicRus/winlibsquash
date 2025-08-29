#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/libsquash/squash.h"

// Рекурсивная функция для вывода содержимого директории
SQUASH_API squash_error_t list_directory_recursive_by_inode(squash_fs_t *fs, squash_off_t inode_ref, const char *display_path, int depth, squash_visited_inodes_t *visited) {
    if (!fs) {
        return SQUASH_ERROR_INVALID_FILE;
    }

    // Проверяем циклы
    if (squash_visited_inodes_contains(visited, inode_ref)) {
        printf("Cycle detected: inode_ref 0x%llx already visited for path %s\n", inode_ref, display_path);
        return SQUASH_OK;
    }

    squash_error_t err = squash_visited_inodes_add(visited, inode_ref);
    if (err != SQUASH_OK) {
        return err;
    }

    void *inode;
    err = squash_read_inode(fs, inode_ref, &inode);
    if (err != SQUASH_OK) {
        fprintf(stderr, "Failed to read inode for '%s': %s\n", display_path, squash_strerror(err));
        return err;
    }

    if (!squash_is_directory(inode)) {
        squash_free_inode(inode);
        fprintf(stderr, "'%s' is not a directory\n", display_path);
        return SQUASH_ERROR_NOT_DIRECTORY;
    }

    // Выводим текущую директорию
    for (int i = 0; i < depth; i++) {
        printf("  ");
    }
    printf("%s/\n", display_path);

    squash_dir_inode_t *dir_inode = (squash_dir_inode_t *)inode;
    squash_dir_iterator_t *iterator;
    err = squash_opendir(fs, dir_inode, &iterator);
    if (err != SQUASH_OK) {
        squash_free_inode(inode);
        fprintf(stderr, "Failed to open directory '%s': %s\n", display_path, squash_strerror(err));
        return err;
    }

    squash_dir_entry_t *entry;
    while (squash_readdir(iterator, &entry) == SQUASH_OK && entry) {
        // Пропускаем "." и ".."
        if (strcmp(entry->name, ".") == 0 || strcmp(entry->name, "..") == 0) {
            squash_free_dir_entry(entry);
            continue;
        }

        // Читаем инод ТОЛЬКО ОДИН РАЗ для определения типа
        void *entry_inode;
        err = squash_read_inode(fs, entry->inode_ref, &entry_inode);
        if (err != SQUASH_OK) {
            fprintf(stderr, "Failed to read inode for entry '%s': %s\n", entry->name, squash_strerror(err));
            squash_free_dir_entry(entry);
            continue;
        }

        // Выводим запись с отступом
        for (int i = 0; i < depth + 1; i++) {
            printf("  ");
        }
        printf("%s%s (inode_ref=0x%llx)\n", entry->name, squash_is_directory(entry_inode) ? "/" : "", entry->inode_ref);

        // Рекурсивный вызов для директорий - ПЕРЕДАЁМ ТОЛЬКО inode_ref!
        if (squash_is_directory(entry_inode)) {
            // Формируем display_path только для красивого вывода
            char *child_display_path = malloc(strlen(display_path) + strlen(entry->name) + 2);
            if (child_display_path) {
                sprintf(child_display_path, "%s/%s", 
                       (strcmp(display_path, ".") == 0) ? "" : display_path, 
                       entry->name);
                
                // ГЛАВНОЕ: используем entry->inode_ref напрямую, без lookup!
                err = list_directory_recursive_by_inode(fs, entry->inode_ref, child_display_path, depth + 1, visited);
                
                free(child_display_path);
                
                if (err != SQUASH_OK) {
                    squash_free_inode(entry_inode);
                    squash_free_dir_entry(entry);
                    squash_closedir(iterator);
                    squash_free_inode(inode);
                    return err;
                }
            }
        }

        squash_free_inode(entry_inode);
        squash_free_dir_entry(entry);
    }

    squash_closedir(iterator);
    squash_free_inode(inode);
    return SQUASH_OK;
}

SQUASH_API squash_error_t list_directory_recursive(squash_fs_t *fs, const char *path, int depth, squash_visited_inodes_t *visited) {
    if (!fs || !path) {
        return SQUASH_ERROR_INVALID_FILE;
    }

    squash_off_t inode_ref;
    squash_error_t err = squash_lookup_path(fs, path, &inode_ref);
    if (err != SQUASH_OK) {
        fprintf(stderr, "Failed to find path '%s': %s\n", path, squash_strerror(err));
        return err;
    }

    // Делаем lookup ТОЛЬКО ОДИН РАЗ для корневого пути
    return list_directory_recursive_by_inode(fs, inode_ref, path, depth, visited);
}

int main(int argc, char *argv[]) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <squashfs_image> [path]\n", argv[0]);
        return 1;
    }

    const char *path = (argc == 3) ? argv[2] : "/";

    squash_fs_t *fs;
    squash_error_t err = squash_open(argv[1], &fs);
    if (err != SQUASH_OK) {
        fprintf(stderr, "Failed to open SquashFS image: %s\n", squash_strerror(err));
        return 1;
    }

    // Инициализируем структуру для отслеживания посещённых inode
    squash_visited_inodes_t visited;
    err = squash_visited_inodes_init(&visited, 16);
    if (err != SQUASH_OK) {
        fprintf(stderr, "Failed to initialize visited inodes: %s\n", squash_strerror(err));
        squash_close(fs);
        return 1;
    }

    err = list_directory_recursive(fs, path, 0, &visited);
    squash_visited_inodes_free(&visited);
    squash_close(fs);

    if (err != SQUASH_OK) {
        return 1;
    }

    return 0;
}