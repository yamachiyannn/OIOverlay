cmake_minimum_required(VERSION 3.15)

cmake_policy(SET CMP0091 NEW)

set(PROJECT_NAME OIOverlay)

project("${PROJECT_NAME}" C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/install")

set(CMAKE_MSVC_RUNTIME_LIBRARY
    "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

option(FORCE_PDB "Generate pdb file for release build" OFF)

add_definitions(
    -DWINVER=0x0501
    -D_WIN32_WINNT=0x0501
)

if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang" AND
    "${CMAKE_CXX_SIMULATE_ID}" STREQUAL "MSVC")
    set(CMAKE_CXX_FLAGS
        "${CMAKE_CXX_FLAGS} -Wno-c++11-narrowing -Wno-microsoft-cast")
endif ()

set(CMAKE_EXE_LINKER_FLAGS
    "${CMAKE_EXE_LINKER_FLAGS} /Brepro"
)

set(CMAKE_MODULE_LINKER_FLAGS
    "${CMAKE_MODULE_LINKER_FLAGS} /Brepro"
)

set(CMAKE_SHARED_LINKER_FLAGS
    "${CMAKE_SHARED_LINKER_FLAGS} /Brepro"
)

if (FORCE_PDB)
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE
        "${CMAKE_EXE_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")

    set(CMAKE_MODULE_LINKER_FLAGS_RELEASE
        "${CMAKE_MODULE_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")

    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE
        "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /DEBUG /OPT:REF /OPT:ICF")
endif ()


# ============================================================
# SokuLib
# ============================================================

add_subdirectory(SokuLib EXCLUDE_FROM_ALL)


# ============================================================
# SQLite
#
# Build time only:
# GitHub Actions downloads the official SQLite amalgamation,
# then compiles sqlite3.c directly into OIOverlay.dll.
#
# The current official SQLite download is 3.53.4.
# ============================================================

set(SQLITE_VERSION 3530400)

set(SQLITE_DIR
    "${CMAKE_BINARY_DIR}/sqlite"
)

set(SQLITE_ARCHIVE
    "${SQLITE_DIR}/sqlite-amalgamation-${SQLITE_VERSION}.zip"
)

set(SQLITE_ROOT
    "${SQLITE_DIR}/sqlite-amalgamation-${SQLITE_VERSION}"
)

if (NOT EXISTS "${SQLITE_ROOT}/sqlite3.c")

    file(MAKE_DIRECTORY "${SQLITE_DIR}")

    file(
        DOWNLOAD
        "https://www.sqlite.org/2026/sqlite-amalgamation-${SQLITE_VERSION}.zip"
        "${SQLITE_ARCHIVE}"
        SHOW_PROGRESS
        STATUS SQLITE_DOWNLOAD_STATUS
    )

    list(GET SQLITE_DOWNLOAD_STATUS 0 SQLITE_DOWNLOAD_RESULT)

    if (NOT SQLITE_DOWNLOAD_RESULT EQUAL 0)
        message(
            FATAL_ERROR
            "SQLite download failed: ${SQLITE_DOWNLOAD_STATUS}"
        )
    endif ()

    execute_process(
        COMMAND
            ${CMAKE_COMMAND}
            -E tar xf
            "${SQLITE_ARCHIVE}"
        WORKING_DIRECTORY
            "${SQLITE_DIR}"
        RESULT_VARIABLE SQLITE_EXTRACT_RESULT
    )

    if (NOT SQLITE_EXTRACT_RESULT EQUAL 0)
        message(
            FATAL_ERROR
            "SQLite extraction failed."
        )
    endif ()

endif ()


add_library(sqlite3 STATIC
    "${SQLITE_ROOT}/sqlite3.c"
)

target_include_directories(sqlite3 PUBLIC
    "${SQLITE_ROOT}"
)

target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_MEMSTATUS=0
)


# ============================================================
# OIOverlay
# ============================================================

add_library(
    "${PROJECT_NAME}"
    MODULE
    src/main.cpp
    src/TskDatabase.cpp
    src/TskDatabase.hpp
    src/version.rc
)

target_compile_options(
    "${PROJECT_NAME}"
    PRIVATE
    /Zi
    /utf-8
)

target_compile_definitions(
    "${PROJECT_NAME}"
    PRIVATE
    DIRECTINPUT_VERSION=0x0800
    CURL_STATICLIB
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    $<$<CONFIG:Debug>:_DEBUG>
)

target_include_directories(
    "${PROJECT_NAME}"
    PRIVATE
    include
    SokuLib/directx
    "src/${PROJECT_NAME}/"
    src
    "${SQLITE_ROOT}"
)

target_link_directories(
    "${PROJECT_NAME}"
    PRIVATE
    lib
)

target_link_libraries(
    "${PROJECT_NAME}"
    sqlite3
    SokuLib
    shlwapi
    d3d9
    d3dx9
    user32
)

set_target_properties(
    "${PROJECT_NAME}"
    PROPERTIES
    PREFIX ""
)

# binary
install(
    TARGETS "${PROJECT_NAME}"
    DESTINATION .
)

# PDB file
install(
    FILES
    $<TARGET_PDB_FILE:${PROJECT_NAME}>
    DESTINATION .
    OPTIONAL
)

# license and other common files
install(
    FILES
    LICENSE
    DESTINATION .
)
