cmake_minimum_required(VERSION 3.10)
project(libsquash C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(BUILD_SHARED OFF)
# Определение библиотеки
add_library(squash
    src/squash_utils.c
    src/squash_errors.c
    src/squash_visited.c
    src/squash_reader.c
    src/squash_decompressor.c
    src/squash_inode.c
    src/squash_directory.c
    src/squash_file.c
)

target_include_directories(squash PUBLIC ${CMAKE_SOURCE_DIR}/include/libsquash)

# Поиск зависимостей
find_package(ZLIB)
if (ZLIB_FOUND)
    target_include_directories(squash PRIVATE ${ZLIB_INCLUDE_DIRS})
    target_link_libraries(squash PRIVATE ${ZLIB_LIBRARIES})
    target_compile_definitions(squash PRIVATE HAVE_ZLIB)
endif()

find_package(LibLZMA)
if(LibLZMA_FOUND)
    message(STATUS "LZMA found: enabling LZMA support")
    target_link_libraries(squash PRIVATE LibLZMA::LibLZMA)
    target_compile_definitions(squash PRIVATE HAVE_LZMA)
    target_compile_definitions(squash PRIVATE HAVE_XZ)
endif()

find_library(LZO_LIBRARY NAMES lzo2)
if (LZO_LIBRARY)
    target_link_libraries(squash PRIVATE ${LZO_LIBRARY})
    target_compile_definitions(squash PRIVATE HAVE_LZO)
endif()

find_package(LZ4 QUIET)
if(LZ4_FOUND)
    message(STATUS "LZ4 found: enabling LZ4 support")
    message(STATUS "LZ4 version: ${LZ4_VERSION}")
        
    get_target_property(LZ4_SHARED_LOCATION LZ4::lz4_shared IMPORTED_LOCATION_RELEASE)
    get_target_property(LZ4_STATIC_LOCATION LZ4::lz4_static IMPORTED_LOCATION_RELEASE)
        
    message(STATUS "LZ4 shared library: ${LZ4_SHARED_LOCATION}")
    message(STATUS "LZ4 static library: ${LZ4_STATIC_LOCATION}")
        
    target_compile_definitions(squash PRIVATE HAVE_LZ4)
    target_link_libraries(squash PRIVATE LZ4::lz4)
else()
    message(FATAL_ERROR "LZ4 requested but not found")
endif()


find_package(zstd QUIET)
if(zstd_FOUND)
    message(STATUS "zstd found: enabling zstd support")
    message(STATUS "zstd version: ${zstd_VERSION}")
        
    get_target_property(ZSTD_SHARED_LOCATION zstd::libzstd_shared IMPORTED_LOCATION_RELEASE)
    get_target_property(ZSTD_STATIC_LOCATION zstd::libzstd_static IMPORTED_LOCATION_RELEASE)
        
    message(STATUS "zstd shared library: ${ZSTD_SHARED_LOCATION}")
    message(STATUS "zstd static library: ${ZSTD_STATIC_LOCATION}")
        
    target_compile_definitions(squash PRIVATE HAVE_ZSTD)
    if(BUILD_SHARED)
        target_link_libraries(squash PRIVATE zstd::libzstd_shared)
    else()
        target_link_libraries(squash PRIVATE zstd::libzstd_static)
    endif()
endif()

find_package(LZO REQUIRED)
if(LZO_FOUND)
    message(STATUS "LZO found: enabling LZO support")
    message(STATUS "LZO version: ${LZO_VERSION}")
        
    get_target_property(LZO_STATIC_LOCATION LZO::lzo_static_lib IMPORTED_LOCATION_RELEASE)
        
    message(STATUS "LZO static library: ${LZO_STATIC_LOCATION}")

    target_compile_definitions(squash PRIVATE HAVE_LZO)
    target_link_libraries(squash PRIVATE LZO::lzo_static_lib)
endif()


# Установка библиотеки
install(TARGETS squash
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib)
install(DIRECTORY include/libsquash/ DESTINATION include/libsquash)

# Примеры
add_executable(squash_ls examples/squash_ls.c)
target_link_libraries(squash_ls PRIVATE squash)

add_executable(squash_extract examples/squash_extract.c)
target_link_libraries(squash_extract PRIVATE squash)

add_executable(squash_info examples/squash_info.c)
target_link_libraries(squash_info PRIVATE squash)

# Установка примеров
install(TARGETS squash_ls squash_extract squash_info
        RUNTIME DESTINATION bin)