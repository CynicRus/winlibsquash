#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/libsquash/squash.h"

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif
#ifdef HAVE_LZMA
#include <lzma.h>
#endif
#ifdef HAVE_LZO
#include <lzo/lzo1x.h>
#endif
#ifdef HAVE_XZ
#include <lzma.h>
#endif
#ifdef HAVE_LZ4
#include <lz4.h>
#endif
#ifdef HAVE_ZSTD
#include <zstd.h>
#endif

// Forward declarations
#ifdef HAVE_ZLIB
static squash_error_t decompress_gzip(const void *compressed_data, size_t compressed_size,
                                      void *uncompressed_data, size_t *uncompressed_size);
#endif
#ifdef HAVE_LZMA
static squash_error_t decompress_lzma(const void *compressed_data, size_t compressed_size,
                                      void *uncompressed_data, size_t *uncompressed_size);
#endif
#ifdef HAVE_LZO
static squash_error_t decompress_lzo(const void *compressed_data, size_t compressed_size,
                                     void *uncompressed_data, size_t *uncompressed_size);
#endif
#ifdef HAVE_XZ
static squash_error_t decompress_xz(const void *compressed_data, size_t compressed_size,
                                    void *uncompressed_data, size_t *uncompressed_size);
#endif
#ifdef HAVE_LZ4
static squash_error_t decompress_lz4(const void *compressed_data, size_t compressed_size,
                                     void *uncompressed_data, size_t *uncompressed_size);
#endif
#ifdef HAVE_ZSTD
static squash_error_t decompress_zstd(const void *compressed_data, size_t compressed_size,
                                      void *uncompressed_data, size_t *uncompressed_size);
#endif

// Внутренняя структура декомпрессора
struct squash_decompressor
{
    squash_compression_t type;
    void *internal_state;
};

SQUASH_API squash_decompressor_t *squash_decompressor_create(squash_compression_t type)
{
    squash_decompressor_t *dec = malloc(sizeof(squash_decompressor_t));
    if (!dec)
        return NULL;

    dec->type = type;
    dec->internal_state = NULL;

    switch (type)
    {
    case SQUASH_COMPRESSION_GZIP:
#ifdef HAVE_ZLIB
    {
        z_stream *strm = malloc(sizeof(z_stream));
        if (!strm)
        {
            free(dec);
            return NULL;
        }
        memset(strm, 0, sizeof(z_stream));
        if (inflateInit2(strm, -15) != Z_OK)
        {
            free(strm);
            free(dec);
            return NULL;
        }
        dec->internal_state = strm;
    }
    break;
#else
        free(dec);
        return NULL;
#endif
    case SQUASH_COMPRESSION_LZMA:
#ifdef HAVE_LZMA
    {
        lzma_stream *strm = malloc(sizeof(lzma_stream));
        if (!strm)
        {
            free(dec);
            return NULL;
        }
        memset(strm, 0, sizeof(lzma_stream));
        if (lzma_alone_decoder(strm, UINT64_MAX) != LZMA_OK)
        {
            free(strm);
            free(dec);
            return NULL;
        }
        dec->internal_state = strm;
    }
    break;
#else
        free(dec);
        return NULL;
#endif
    case SQUASH_COMPRESSION_LZO:
#ifdef HAVE_LZO
        if (lzo_init() != LZO_E_OK)
        {
            free(dec);
            return NULL;
        }
        dec->internal_state = NULL; // LZO не требует постоянного состояния
        break;
#else
        free(dec);
        return NULL;
#endif
    case SQUASH_COMPRESSION_XZ:
#ifdef HAVE_XZ
    {
        lzma_stream *strm = malloc(sizeof(lzma_stream));
        if (!strm)
        {
            free(dec);
            return NULL;
        }
        memset(strm, 0, sizeof(lzma_stream));
        if (lzma_stream_decoder(strm, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK)
        {
            free(strm);
            free(dec);
            return NULL;
        }
        dec->internal_state = strm;
    }
    break;
#else
        free(dec);
        return NULL;
#endif
    case SQUASH_COMPRESSION_LZ4:
#ifdef HAVE_LZ4
        dec->internal_state = NULL; // LZ4 не требует постоянного состояния
        break;
#else
        free(dec);
        return NULL;
#endif
    case SQUASH_COMPRESSION_ZSTD:
#ifdef HAVE_ZSTD
    {
        ZSTD_DCtx *ctx = ZSTD_createDCtx();
        if (!ctx)
        {
            free(dec);
            return NULL;
        }
        dec->internal_state = ctx;
    }
    break;
#else
        free(dec);
        return NULL;
#endif
    default:
        free(dec);
        return NULL;
    }

    return dec;
}

SQUASH_API void squash_decompressor_destroy(squash_decompressor_t *dec)
{
    if (!dec)
        return;

    if (dec->internal_state)
    {
        switch (dec->type)
        {
        case SQUASH_COMPRESSION_GZIP:
#ifdef HAVE_ZLIB
            inflateEnd((z_stream *)dec->internal_state);
            free(dec->internal_state);
#endif
            break;
        case SQUASH_COMPRESSION_LZMA:
#ifdef HAVE_LZMA
            lzma_end((lzma_stream *)dec->internal_state);
            free(dec->internal_state);
#endif
            break;
        case SQUASH_COMPRESSION_XZ:
#ifdef HAVE_XZ
            lzma_end((lzma_stream *)dec->internal_state);
            free(dec->internal_state);
#endif
            break;
        case SQUASH_COMPRESSION_ZSTD:
#ifdef HAVE_ZSTD
            ZSTD_freeDCtx((ZSTD_DCtx *)dec->internal_state);
#endif
            break;
        default:
            break;
        }
    }

    free(dec);
}

SQUASH_API squash_error_t squash_decompress_block(
    squash_decompressor_t *dec,
    const void *compressed_data,
    size_t compressed_size,
    void *uncompressed_data,
    size_t *uncompressed_size)
{
    if (!dec || !compressed_data || !uncompressed_data || !uncompressed_size)
    {
        return SQUASH_ERROR_INVALID_FILE;
    }

    switch (dec->type)
    {
    case SQUASH_COMPRESSION_GZIP:
#ifdef HAVE_ZLIB
        return decompress_gzip(compressed_data, compressed_size,
                               uncompressed_data, uncompressed_size);
#else
        return SQUASH_ERROR_COMPRESSION_NOT_SUPPORTED;
#endif
    case SQUASH_COMPRESSION_LZMA:
#ifdef HAVE_LZMA
        return decompress_lzma(compressed_data, compressed_size,
                               uncompressed_data, uncompressed_size);
#else
        return SQUASH_ERROR_COMPRESSION_NOT_SUPPORTED;
#endif
    case SQUASH_COMPRESSION_LZO:
#ifdef HAVE_LZO
        return decompress_lzo(compressed_data, compressed_size,
                              uncompressed_data, uncompressed_size);
#else
        return SQUASH_ERROR_COMPRESSION_NOT_SUPPORTED;
#endif
    case SQUASH_COMPRESSION_XZ:
#ifdef HAVE_XZ
        return decompress_xz(compressed_data, compressed_size,
                             uncompressed_data, uncompressed_size);
#else
        return SQUASH_ERROR_COMPRESSION_NOT_SUPPORTED;
#endif
    case SQUASH_COMPRESSION_LZ4:
#ifdef HAVE_LZ4
        return decompress_lz4(compressed_data, compressed_size,
                              uncompressed_data, uncompressed_size);
#else
        return SQUASH_ERROR_COMPRESSION_NOT_SUPPORTED;
#endif
    case SQUASH_COMPRESSION_ZSTD:
#ifdef HAVE_ZSTD
        return decompress_zstd(compressed_data, compressed_size,
                               uncompressed_data, uncompressed_size);
#else
        return SQUASH_ERROR_COMPRESSION_NOT_SUPPORTED;
#endif
    default:
        return SQUASH_ERROR_COMPRESSION_NOT_SUPPORTED;
    }
}

#ifdef HAVE_ZLIB
static squash_error_t decompress_gzip(
    const void *compressed_data,
    size_t compressed_size,
    void *uncompressed_data,
    size_t *uncompressed_size)
{
    z_stream strm = {0};
    int ret;

    strm.next_in = (Bytef *)compressed_data;
    strm.avail_in = compressed_size;
    strm.next_out = (Bytef *)uncompressed_data;
    strm.avail_out = *uncompressed_size;

    ret = inflateInit2(&strm, 15 + 32); // +32 для gzip формата
    if (ret != Z_OK)
    {
        return SQUASH_ERROR_DECOMPRESSION_FAILED;
    }

    ret = inflate(&strm, Z_FINISH);
    if (ret != Z_STREAM_END)
    {
        inflateEnd(&strm);
        return SQUASH_ERROR_DECOMPRESSION_FAILED;
    }

    *uncompressed_size = strm.total_out;
    inflateEnd(&strm);

    return SQUASH_OK;
}
#endif

#ifdef HAVE_LZMA
static squash_error_t decompress_lzma(
    const void *compressed_data,
    size_t compressed_size,
    void *uncompressed_data,
    size_t *uncompressed_size
) {
    lzma_stream strm = LZMA_STREAM_INIT;
    lzma_ret ret;

    // Проверяем входные параметры
    if (!compressed_data || !uncompressed_data || !uncompressed_size || compressed_size < 5) {
        printf("Invalid parameters: compressed_data=%p, uncompressed_data=%p, uncompressed_size=%zu, compressed_size=%zu\n",
               compressed_data, uncompressed_data, *uncompressed_size, compressed_size);
        return SQUASH_ERROR_DECOMPRESSION_FAILED;
    }

    // Считываем заголовок (5 байт)
    uint8_t props = ((const uint8_t *)compressed_data)[0];
    uint32_t lc = props % 9;
    uint32_t remainder = props / 9;
    uint32_t lp = remainder % 5;
    uint32_t pb = remainder / 5;
    uint32_t dict_size = ((const uint8_t *)compressed_data)[1] |
                         ((const uint8_t *)compressed_data)[2] << 8 |
                         ((const uint8_t *)compressed_data)[3] << 16 |
                         ((const uint8_t *)compressed_data)[4] << 24;

    // Проверяем корректность параметров
    if (lc > 4 || lp > 4 || pb > 4 || (lc + lp) > 4 || dict_size == 0) {
        printf("Invalid LZMA properties: lc=%u, lp=%u, pb=%u, dict_size=%u\n", lc, lp, pb, dict_size);
        return SQUASH_ERROR_DECOMPRESSION_FAILED;
    }

    // Для отладки: выводим параметры и первые 16 байт данных
   /* printf("LZMA decompress: compressed_size=%zu, expected_uncompressed=%zu\n", 
           compressed_size, *uncompressed_size);
    printf("LZMA properties: lc=%u, lp=%u, pb=%u, dict_size=%u\n", lc, lp, pb, dict_size);
    printf("First 16 bytes: ");
    for (int i = 0; i < 16 && i < compressed_size; i++) {
        printf("%02X ", ((const uint8_t *)compressed_data)[i]);
    }
    printf("\n");*/

    // Настраиваем параметры LZMA1
    lzma_options_lzma opt_lzma = {
        .dict_size = dict_size,
        .lc = lc,
        .lp = lp,
        .pb = pb,
    };
    lzma_filter filters[] = {
        { .id = LZMA_FILTER_LZMA1, .options = &opt_lzma },
        { .id = LZMA_VLI_UNKNOWN, .options = NULL }
    };

    // Инициализация декодера
    ret = lzma_raw_decoder(&strm, filters);
    if (ret != LZMA_OK) {
        printf("lzma_raw_decoder failed: %d\n", ret);
        lzma_end(&strm);
        return SQUASH_ERROR_DECOMPRESSION_FAILED;
    }

    // Устанавливаем входные и выходные буферы, пропуская заголовок
    strm.next_in = (const uint8_t *)compressed_data + 5;
    strm.avail_in = compressed_size - 5;
    strm.next_out = (uint8_t *)uncompressed_data;
    strm.avail_out = *uncompressed_size;

    // Декомпрессия
    ret = lzma_code(&strm, LZMA_RUN);
    if (ret != LZMA_STREAM_END && ret != LZMA_OK) {
        printf("lzma_code failed: %d\n", ret);
        lzma_end(&strm);
        return SQUASH_ERROR_DECOMPRESSION_FAILED;
    }

    // Завершаем декомпрессию
    if (ret != LZMA_STREAM_END) {
        ret = lzma_code(&strm, LZMA_FINISH);
        if (ret != LZMA_STREAM_END) {
            printf("lzma_code did not reach LZMA_STREAM_END: %d\n", ret);
            lzma_end(&strm);
            return SQUASH_ERROR_DECOMPRESSION_FAILED;
        }
    }

    // Обновляем размер распакованных данных
    printf("Decompression successful: %zu -> %zu bytes\n", 
           strm.total_in, strm.total_out);
    *uncompressed_size = strm.total_out;
    lzma_end(&strm);
    return SQUASH_OK;
}
#endif

#ifdef HAVE_LZO
static squash_error_t decompress_lzo(
    const void *compressed_data,
    size_t compressed_size,
    void *uncompressed_data,
    size_t *uncompressed_size)
{
    lzo_uint out_len = *uncompressed_size;
    int ret = lzo1x_decompress_safe(compressed_data, compressed_size,
                                    uncompressed_data, &out_len, NULL);
    if (ret != LZO_E_OK)
    {
        return SQUASH_ERROR_DECOMPRESSION_FAILED;
    }

    *uncompressed_size = out_len;
    return SQUASH_OK;
}
#endif

#ifdef HAVE_XZ
static squash_error_t decompress_xz(
    const void *compressed_data,
    size_t compressed_size,
    void *uncompressed_data,
    size_t *uncompressed_size)
{
    if (!compressed_data || !uncompressed_data || !uncompressed_size)
    {
        return SQUASH_ERROR_INVALID_ARGUMENT;
    }

    lzma_stream strm = LZMA_STREAM_INIT;

    // Инициализация с более строгими параметрами
    lzma_ret ret = lzma_auto_decoder(&strm, 128 * 1024 * 1024, 0); // 128MB лимит

    if (ret != LZMA_OK)
    {
        return (ret == LZMA_MEM_ERROR) ? SQUASH_ERROR_MEMORY : SQUASH_ERROR_DECOMPRESSION_FAILED;
    }

    strm.next_in = (const uint8_t *)compressed_data;
    strm.avail_in = compressed_size;
    strm.next_out = (uint8_t *)uncompressed_data;
    strm.avail_out = *uncompressed_size;

    size_t original_out_size = *uncompressed_size;

    // Потоковое декодирование с проверкой прогресса
    lzma_action action = LZMA_RUN;

    do
    {
        // Если входные данные закончились, переключаемся на финализацию
        if (strm.avail_in == 0)
        {
            action = LZMA_FINISH;
        }

        ret = lzma_code(&strm, action);

        // Проверяем на зацикливание (отсутствие прогресса)
        if (ret == LZMA_OK && strm.avail_out == *uncompressed_size && action == LZMA_FINISH)
        {
            lzma_end(&strm);
            return SQUASH_ERROR_DECOMPRESSION_FAILED;
        }

    } while (ret == LZMA_OK);

    *uncompressed_size = original_out_size - strm.avail_out;
    lzma_end(&strm);

    // Обработка результатов аналогично предыдущей версии
    if (ret == LZMA_STREAM_END)
    {
        return SQUASH_OK;
    }
    else if (ret == LZMA_BUF_ERROR)
    {
        return SQUASH_ERROR_DECOMPRESSION_FAILED;
    }
    else if (ret == LZMA_DATA_ERROR || ret == LZMA_FORMAT_ERROR)
    {
        return SQUASH_ERROR_DECOMPRESSION_FAILED;
    }
    else if (ret == LZMA_MEM_ERROR)
    {
        return SQUASH_ERROR_MEMORY;
    }
    else
    {
        return SQUASH_ERROR_DECOMPRESSION_FAILED;
    }
}
#endif

#ifdef HAVE_LZ4
static squash_error_t decompress_lz4(
    const void *compressed_data,
    size_t compressed_size,
    void *uncompressed_data,
    size_t *uncompressed_size)
{
    int ret = LZ4_decompress_safe(compressed_data, uncompressed_data,
                                  compressed_size, *uncompressed_size);
    if (ret < 0)
    {
        return SQUASH_ERROR_DECOMPRESSION_FAILED;
    }

    *uncompressed_size = ret;
    return SQUASH_OK;
}
#endif

#ifdef HAVE_ZSTD
static squash_error_t decompress_zstd(
    const void *compressed_data,
    size_t compressed_size,
    void *uncompressed_data,
    size_t *uncompressed_size)
{
    if (!compressed_data || !uncompressed_data || !uncompressed_size)
    {
        return SQUASH_ERROR_INVALID_ARGUMENT;
    }

    size_t ret = ZSTD_decompress(uncompressed_data, *uncompressed_size,
                                 compressed_data, compressed_size);
    
    if (ZSTD_isError(ret))
    {
        printf("ZSTD decompression failed: %s\n", ZSTD_getErrorName(ret));
        return SQUASH_ERROR_DECOMPRESSION_FAILED;
    }

    *uncompressed_size = ret;
    return SQUASH_OK;
}
#endif