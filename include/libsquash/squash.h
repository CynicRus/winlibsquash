#ifndef SQUASH_H
#define SQUASH_H

#include "squash_types.h"
#include "squash_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

// Основные функции для работы с образом
SQUASH_API squash_error_t squash_open(const char *filename, squash_fs_t **fs);
SQUASH_API void squash_close(squash_fs_t *fs);
SQUASH_API squash_error_t squash_get_super(squash_fs_t *fs, squash_super_t *super);

// Функции для работы с декомпрессором
SQUASH_API squash_decompressor_t* squash_decompressor_create(squash_compression_t type);
SQUASH_API void squash_decompressor_destroy(squash_decompressor_t *dec);
SQUASH_API squash_error_t squash_decompress_block(
    squash_decompressor_t *dec,
    const void *compressed_data,
    size_t compressed_size,
    void *uncompressed_data,
    size_t *uncompressed_size
);

// Функции для работы с инодами
SQUASH_API squash_error_t squash_read_inode(squash_fs_t *fs, squash_off_t inode_ref, void **inode);
SQUASH_API squash_error_t squash_lookup_path(squash_fs_t *fs, const char *path, squash_off_t *inode_ref);
SQUASH_API void squash_free_inode(void *inode);

// Функции для работы с файлами
SQUASH_API squash_error_t squash_read_file(squash_fs_t *fs, squash_reg_inode_t *inode, 
                                          void *buffer, size_t offset, size_t size, size_t *bytes_read);
SQUASH_API squash_error_t squash_get_file_size(squash_reg_inode_t *inode, uint64_t *size);

// Функции для работы с директориями
SQUASH_API squash_error_t squash_opendir(squash_fs_t *fs, squash_dir_inode_t *dir_inode, 
                                        squash_dir_iterator_t **iterator);
SQUASH_API squash_error_t squash_readdir(squash_dir_iterator_t *iterator, squash_dir_entry_t **entry);
SQUASH_API void squash_closedir(squash_dir_iterator_t *iterator);
SQUASH_API void squash_free_dir_entry(squash_dir_entry_t *entry);

// Утилитарные функции
SQUASH_API squash_error_t squash_extract_file(squash_fs_t *fs, const char *path, const char *output_path);
SQUASH_API squash_error_t squash_extract_directory(squash_fs_t *fs, const char *path, const char *output_dir);
SQUASH_API squash_error_t squash_list_directory(squash_fs_t *fs, const char *path);

// Информационные функции
SQUASH_API const char* squash_get_compression_name(uint16_t compression);
SQUASH_API bool squash_is_file(void *inode);
SQUASH_API bool squash_is_directory(void *inode);
SQUASH_API bool squash_is_symlink(void *inode);

// Функции для работы с visited_inodes
squash_error_t squash_visited_inodes_init(squash_visited_inodes_t *visited, size_t initial_capacity);
void squash_visited_inodes_free(squash_visited_inodes_t *visited);
squash_error_t squash_visited_inodes_add(squash_visited_inodes_t *visited, squash_off_t inode_ref);
bool squash_visited_inodes_contains(squash_visited_inodes_t *visited, squash_off_t inode_ref);

//вспомогательные функции чтения данных 
squash_error_t read_fs_bytes(FILE *file, uint64_t start, size_t bytes, void *buffer);
squash_error_t squash_read_metadata_block(squash_fs_t *fs, squash_off_t offset, uint8_t **uncompressed_data, size_t *uncompressed_size, size_t *compressed_size);
squash_error_t squash_read_data_block(squash_fs_t *fs, squash_off_t offset,
                                     uint32_t compressed_size, bool is_compressed,
                                     uint8_t **uncompressed_data, size_t *uncompressed_size);
squash_error_t read_n_bytes_from_metablocks(squash_fs_t *fs, uint64_t start_offset, size_t offset_in_block,
                                            size_t n_bytes, uint8_t *out_buf, uint64_t *next_offset);

#ifdef __cplusplus
}
#endif

#endif // SQUASH_H