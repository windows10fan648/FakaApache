cmake_minimum_required(VERSION 3.20)

option(FAKAAPACHE_BOOTSTRAP_VCPKG "Download and bootstrap vcpkg automatically" ON)
set(FAKAAPACHE_VCPKG_TAG "2026.10.16" CACHE STRING "Pinned vcpkg tag used for automatic bootstrapping")
if(FAKAAPACHE_VCPKG_TAG STREQUAL "2024.11.16")
    set(FAKAAPACHE_VCPKG_TAG "2026.10.16" CACHE STRING "Pinned vcpkg tag used for automatic bootstrapping" FORCE)
endif()
set(FAKAAPACHE_VCPKG_ROOT "${CMAKE_BINARY_DIR}/_vcpkg-${FAKAAPACHE_VCPKG_TAG}" CACHE PATH "vcpkg checkout used by FakaApache")
if(FAKAAPACHE_VCPKG_ROOT STREQUAL "${CMAKE_BINARY_DIR}/_vcpkg")
    set(FAKAAPACHE_VCPKG_ROOT "${CMAKE_BINARY_DIR}/_vcpkg-${FAKAAPACHE_VCPKG_TAG}" CACHE PATH "vcpkg checkout used by FakaApache" FORCE)
endif()
if(CMAKE_TOOLCHAIN_FILE STREQUAL "${CMAKE_BINARY_DIR}/_vcpkg/scripts/buildsystems/vcpkg.cmake")
    unset(CMAKE_TOOLCHAIN_FILE CACHE)
endif()

if(FAKAAPACHE_BOOTSTRAP_VCPKG AND (NOT DEFINED CMAKE_TOOLCHAIN_FILE OR CMAKE_TOOLCHAIN_FILE STREQUAL ""))
    set(FAKAAPACHE_VCPKG_EXECUTABLE "${FAKAAPACHE_VCPKG_ROOT}/vcpkg")
    if(CMAKE_HOST_WIN32)
        set(FAKAAPACHE_VCPKG_EXECUTABLE "${FAKAAPACHE_VCPKG_ROOT}/vcpkg.exe")
    endif()

    if(NOT EXISTS "${FAKAAPACHE_VCPKG_EXECUTABLE}")
        find_program(FAKAAPACHE_GIT_EXECUTABLE git)
        if(NOT FAKAAPACHE_GIT_EXECUTABLE)
            message(FATAL_ERROR "Git is required to bootstrap vcpkg automatically. Install Git or set FAKAAPACHE_BOOTSTRAP_VCPKG=OFF and provide CMAKE_TOOLCHAIN_FILE.")
        endif()

        if(NOT EXISTS "${FAKAAPACHE_VCPKG_ROOT}")
            message(STATUS "Downloading vcpkg ${FAKAAPACHE_VCPKG_TAG} into ${FAKAAPACHE_VCPKG_ROOT}")
            execute_process(
                COMMAND "${FAKAAPACHE_GIT_EXECUTABLE}" clone --depth 1 --branch "${FAKAAPACHE_VCPKG_TAG}" https://github.com/microsoft/vcpkg.git "${FAKAAPACHE_VCPKG_ROOT}"
                RESULT_VARIABLE FAKAAPACHE_VCPKG_CLONE_RESULT
            )
            if(NOT FAKAAPACHE_VCPKG_CLONE_RESULT EQUAL 0)
                message(FATAL_ERROR "Failed to download vcpkg.")
            endif()
        endif()

        if(CMAKE_HOST_WIN32)
            execute_process(
                COMMAND cmd /c bootstrap-vcpkg.bat -disableMetrics
                WORKING_DIRECTORY "${FAKAAPACHE_VCPKG_ROOT}"
                RESULT_VARIABLE FAKAAPACHE_VCPKG_BOOTSTRAP_RESULT
            )
        else()
            execute_process(
                COMMAND "${CMAKE_COMMAND}" -E env bash ./bootstrap-vcpkg.sh -disableMetrics
                WORKING_DIRECTORY "${FAKAAPACHE_VCPKG_ROOT}"
                RESULT_VARIABLE FAKAAPACHE_VCPKG_BOOTSTRAP_RESULT
            )
        endif()
        if(NOT FAKAAPACHE_VCPKG_BOOTSTRAP_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to bootstrap vcpkg.")
        endif()
    endif()

    set(CMAKE_TOOLCHAIN_FILE
        "${FAKAAPACHE_VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "vcpkg toolchain file" FORCE)
endif()

project(FakaApache LANGUAGES CXX)

find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
find_package(ZLIB REQUIRED)

add_executable(fakaapache webserver.cpp)
target_compile_features(fakaapache PRIVATE cxx_std_17)
target_compile_definitions(fakaapache PRIVATE FAKAAPACHE_WITH_OPENSSL)

if(MSVC)
    target_compile_options(fakaapache PRIVATE /W4 /permissive-)
else()
    target_compile_options(fakaapache PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    target_compile_options(fakaapache PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-pthread>)
    target_link_options(fakaapache PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-pthread>)
endif()

if(WIN32)
    target_link_libraries(fakaapache PRIVATE ws2_32 OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)
else()
    target_link_libraries(fakaapache PRIVATE OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)
endif()
