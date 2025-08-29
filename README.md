

# libsquash - SquashFS Reader Library

A lightweight C library for reading and unpacking SquashFS v4.0 filesystem images on Windows.

## Features

- **Read-only SquashFS v4.x support**: Parse and read SquashFS filesystem images
- **Multiple compression formats**: GZIP, LZMA, LZO, XZ, LZ4, ZSTD
- **File extraction**: Extract individual files or entire directories
- **Directory traversal**: List and navigate directory contents
- **Fragment support**: Handle fragmented files for optimal storage
- **Memory efficient**: Stream-based reading with minimal memory footprint

## Supported Compression Formats

| Format | ID | Library Required |
|--------|----|------------------|
| GZIP   | 1  | zlib             |
| LZMA   | 2  | liblzma          |
| LZO    | 3  | lzo              |
| XZ     | 4  | liblzma          |
| LZ4    | 5  | liblz4           |
| ZSTD   | 6  | libzstd          |

## Quick Start

### Opening a SquashFS image

```c
#include "libsquash/squash.h"

squash_fs_t *fs;
squash_error_t err = squash_open("example.squashfs", &fs);
if (err != SQUASH_OK) {
    printf("Error opening: %s\n", squash_strerror(err));
    return -1;
}

// Get filesystem info
squash_super_t super;
squash_get_super(fs, &super);
printf("Block size: %u\n", super.block_size);
printf("Compression: %s\n", squash_get_compression_name(super.compression));

// Clean up
squash_close(fs);
```

### Extracting a file

```c
// Extract a specific file
err = squash_extract_file(fs, "/path/to/file.txt", "output.txt");
if (err != SQUASH_OK) {
    printf("Extract failed: %s\n", squash_strerror(err));
}
```

### Listing directory contents

```c
// Find directory inode
squash_off_t inode_ref;
err = squash_lookup_path(fs, "/some/directory", &inode_ref);
if (err != SQUASH_OK) {
    printf("Path not found: %s\n", squash_strerror(err));
    return;
}

// Read directory inode
void *inode;
err = squash_read_inode(fs, inode_ref, &inode);
if (err != SQUASH_OK || !squash_is_directory(inode)) {
    printf("Not a directory\n");
    return;
}

// Open directory for reading
squash_dir_iterator_t *iterator;
err = squash_opendir(fs, (squash_dir_inode_t*)inode, &iterator);
if (err == SQUASH_OK) {
    squash_dir_entry_t *entry;
    while (squash_readdir(iterator, &entry) == SQUASH_OK && entry) {
        printf("%s\n", entry->name);
        squash_free_dir_entry(entry);
    }
    squash_closedir(iterator);
}
squash_free_inode(inode);
```

### Reading file data

```c
// Read file content into memory
squash_off_t file_inode_ref;
err = squash_lookup_path(fs, "/path/to/file.txt", &file_inode_ref);
if (err == SQUASH_OK) {
    void *inode;
    err = squash_read_inode(fs, file_inode_ref, &inode);
    if (err == SQUASH_OK && squash_is_file(inode)) {
        squash_reg_inode_t *file_inode = (squash_reg_inode_t*)inode;
        
        uint64_t file_size;
        squash_get_file_size(file_inode, &file_size);
        
        char *buffer = malloc(file_size + 1);
        size_t bytes_read;
        err = squash_read_file(fs, file_inode, buffer, 0, file_size, &bytes_read);
        if (err == SQUASH_OK) {
            buffer[bytes_read] = '\0';
            printf("File content: %s\n", buffer);
        }
        free(buffer);
    }
    squash_free_inode(inode);
}
```

### Extracting entire directory

```c
// Extract directory recursively
err = squash_extract_directory(fs, "/usr/share", "./extracted");
if (err != SQUASH_OK) {
    printf("Directory extraction failed: %s\n", squash_strerror(err));
}
```

## API Reference

### Core Functions

| Function            | Description                           |
|--------------------|---------------------------------------|
| `squash_open()`    | Open a SquashFS image file            |
| `squash_close()`   | Close filesystem and free resources   |
| `squash_get_super()` | Get superblock information          |

### File Operations

| Function               | Description                           |
|-----------------------|---------------------------------------|
| `squash_lookup_path()` | Find inode reference by path          |
| `squash_read_inode()` | Read inode from reference            |
| `squash_read_file()`  | Read file data into buffer           |
| `squash_get_file_size()` | Get file size                     |
| `squash_extract_file()` | Extract file to disk               |

### Directory Operations

| Function                   | Description                           |
|---------------------------|---------------------------------------|
| `squash_opendir()`        | Open directory for reading            |
| `squash_readdir()`        | Read next directory entry            |
| `squash_closedir()`       | Close directory iterator              |
| `squash_extract_directory()` | Extract directory recursively       |

### Utility Functions

| Function               | Description                           |
|-----------------------|---------------------------------------|
| `squash_is_file()`    | Check if inode is a regular file      |
| `squash_is_directory()` | Check if inode is a directory       |
| `squash_is_symlink()` | Check if inode is a symbolic link    |
| `squash_strerror()`   | Get error description string          |

## Error Handling

All functions return `squash_error_t` values:

```c
typedef enum {
    SQUASH_OK = 0,
    SQUASH_ERROR_INVALID_FILE,
    SQUASH_ERROR_INVALID_MAGIC,
    SQUASH_ERROR_UNSUPPORTED_VERSION,
    SQUASH_ERROR_MEMORY,
    SQUASH_ERROR_IO,
    SQUASH_ERROR_COMPRESSION,
    SQUASH_ERROR_NOT_FOUND,
    SQUASH_ERROR_NOT_FILE,
    SQUASH_ERROR_NOT_DIRECTORY,
    // ... more error codes
} squash_error_t;
```

Use `squash_strerror()` to get human-readable error messages.

## Building

### Requirements

- C compiler (GCC, Clang, MSVC)
- CMake (optional, for build system)
- Compression libraries (zlib, liblzma, etc.)

### Compilation flags

```bash
# Basic compilation
gcc -c squash_*.c -I./include

# With compression support
gcc -DHAVE_ZLIB -DHAVE_LZMA squash_*.c -lz -llzma

# Windows with MinGW
gcc -DHAVE_ZLIB squash_*.c -lz -o example.exe
```

## Memory Management

The library manages memory automatically for most operations. Key points:

- Always call cleanup functions: `squash_close()`, `squash_free_inode()`, `squash_free_dir_entry()`
- Directory entries must be freed after use with `squash_free_dir_entry()`
- Inodes must be freed with `squash_free_inode()`
- File buffers are managed by the caller

## Thread Safety

This library is not thread-safe. If you need concurrent access, implement your own synchronization.

## Limitations

- **Read-only**: Cannot create or modify SquashFS images
- **SquashFS v4.0 only**: Earlier versions not supported
- **No extended attributes**: xattr support not implemented
- **Windows paths**: Use forward slashes `/` in paths

## License
This project is licensed under the MIT License.

```text
MIT License

Copyright (c) 2025 Aleksandr Vorobev aka CynicRus

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Contributing

Contributions welcome! Please:

1. Fork the repository
2. Create a feature branch
3. Make your changes
4. Add tests or sample
5. Submit a pull request

