# IDE code insight only (CLion): gives the extension sources a project target with the Zend API
# headers on the include path. The extension itself is built with phpize — see
# `make build` / `make docker-build`; this file is never used to produce meminfo.so.
#
# Headers come from the Docker image of the selected version: make ide PHP_VERSION=X.Y
# Pick the matching CMake profile php-X.Y from CMakePresets.json.

cmake_minimum_required(VERSION 3.21)

project(meminfo C)

set(CMAKE_C_STANDARD 99)

set(PHP_VERSION "8.1" CACHE STRING "PHP version whose headers are used for code insight")
set(PHP_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/.ide/php-${PHP_VERSION}/include/php"
    CACHE PATH "PHP include directory (php-config --include-dir)")

if(NOT EXISTS "${PHP_INCLUDE_DIR}/main/php.h")
    message(FATAL_ERROR
        "PHP ${PHP_VERSION} headers not found in ${PHP_INCLUDE_DIR}. "
        "Run: make ide PHP_VERSION=${PHP_VERSION}")
endif()

message(STATUS "meminfo IDE model: PHP ${PHP_VERSION} headers from ${PHP_INCLUDE_DIR}")

# Same sources as PHP_NEW_EXTENSION in extension/config.m4
add_library(meminfo OBJECT
    extension/meminfo.c
    extension/writer.c
    extension/ptr_set.c
    extension/traverse.c
    extension/items.c
    extension/fibers.c
    extension/signal.c
    extension/procinfo.c
    extension/trigger.c
    extension/generations.c
    extension/sites.c
    extension/upload.c
    extension/upload_s3.c
    extension/internal.h
    extension/php_meminfo.h
)

# Same list as php-config --includes
target_include_directories(meminfo PRIVATE
    extension
    ${PHP_INCLUDE_DIR}
    ${PHP_INCLUDE_DIR}/main
    ${PHP_INCLUDE_DIR}/TSRM
    ${PHP_INCLUDE_DIR}/Zend
    ${PHP_INCLUDE_DIR}/ext
    ${PHP_INCLUDE_DIR}/ext/date/lib
)

# Built as a shared extension by phpize, so the ZEND_GET_MODULE block is active
target_compile_definitions(meminfo PRIVATE COMPILE_DL_MEMINFO=1)

# The optional S3 transport (--with-meminfo-s3). With MEMINFO_S3=ON and the vcpkg
# toolchain (preset php-X.Y-s3), aws-c-s3 comes from extension/deps/vcpkg.json and upload_s3.c
# is indexed with MEMINFO_HAVE_S3 on.
option(MEMINFO_S3 "Index the S3 transport against aws-c-s3 from vcpkg" OFF)
if(MEMINFO_S3)
    message(STATUS "meminfo IDE model: S3 manifest ${VCPKG_MANIFEST_DIR}/vcpkg.json")
    find_package(aws-c-s3 CONFIG REQUIRED)
    target_link_libraries(meminfo PRIVATE AWS::aws-c-s3)
    target_compile_definitions(meminfo PRIVATE MEMINFO_HAVE_S3=1)
    message(STATUS "meminfo IDE model: S3 transport ON")
else()
    message(STATUS "meminfo IDE model: S3 transport OFF (preset php-X.Y-s3 turns it on)")
endif()
