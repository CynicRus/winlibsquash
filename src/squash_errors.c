#include "../include/libsquash/squash_errors.h"

SQUASH_API const char* squash_strerror(squash_error_t error) {
    switch (error) {
        case SQUASH_OK: return "Success";
        case SQUASH_ERROR_INVALID_FILE: return "Invalid file or parameters";
        case SQUASH_ERROR_INVALID_MAGIC: return "Invalid SquashFS magic number";
        case SQUASH_ERROR_UNSUPPORTED_VERSION: return "Unsupported SquashFS version";
        case SQUASH_ERROR_MEMORY: return "Memory allocation failed";
        case SQUASH_ERROR_IO: return "Input/output error";
        case SQUASH_ERROR_COMPRESSION: return "Compression error";
        case SQUASH_ERROR_INVALID_INODE: return "Invalid inode";
        case SQUASH_ERROR_NOT_FOUND: return "File or directory not found";
        case SQUASH_ERROR_INVALID_PATH: return "Invalid path";
        case SQUASH_ERROR_PERMISSION: return "Permission denied";
        case SQUASH_ERROR_NOT_DIRECTORY: return "Not a directory";
        case SQUASH_ERROR_NOT_FILE: return "Not a file";
        case SQUASH_ERROR_COMPRESSION_NOT_SUPPORTED: return "Compression not supported";
        case SQUASH_ERROR_DECOMPRESSION_FAILED: return "Decompression failed";
        case SQUASH_ERROR_INVALID_ARGUMENT : return "Invalid argument";
        case SQUASH_ERROR_INVALID_BLOCK : return "Invalid block";
        case SQUASH_ERROR_INVALID_INDEX : return "Invalid index";
        case SQUASH_ERROR_CYCLE_DETECTED : return "Cycle detected";
        default: return "Unknown error";
    }
}