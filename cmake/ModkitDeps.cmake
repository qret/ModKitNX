# cmake/ModkitDeps.cmake — third-party sources for the .nro (CMakeLists.txt) and the host tests (tests/host).
#
# Pinned versions only, each archive checked against a pinned SHA-256 before it is used (CMake stops on a
# mismatch). Where the sources come from, in this order:
#   1. -DMODKIT_DEPS_DIR=<folder> — a folder with zstd/, lz4/, nlohmann/nlohmann/json.hpp, borealis/
#      (the same layout as third_party/). Given explicitly, it is the only source: a missing item is an error,
#      nothing is downloaded (an offline build).
#   2. <repo>/third_party/<name>, if it is already there (a local checkout that has the sources).
#   3. Otherwise the pinned archive is downloaded into the build folder (<build>/_deps) — once per build folder.
#
# | name     | version / commit                           | license                         | used by          |
# |----------|--------------------------------------------|---------------------------------|------------------|
# | zstd     | 1.5.7 (release tarball)                    | BSD-3-Clause (or GPL-2.0)       | core: ZBIC, zstd |
# | lz4      | 1.10.0 (release tarball)                   | BSD-2-Clause (lib/)             | core: NSO        |
# | json     | nlohmann/json 3.11.3 (single json.hpp)     | MIT                             | core: receipt    |
# | borealis | xfangfang fork, 5f08b286f3df737f3321d2247a6fe633fcead03c | Apache-2.0        | ui (.nro only)   |
#
# zstd: only lib/ is compiled, and lib/common/entropy_common.c is NOT compiled on its own — it is #included by
# core/src/zstd_bic_glue.c (FSE_readNCount renamed, BIC tables for ZBIC frames). Keep the source lists in
# CMakeLists.txt and tests/host/CMakeLists.txt identical.
#
# Usage:  include(<repo>/cmake/ModkitDeps.cmake)   then   modkit_deps([BOREALIS])
# Sets:   MODKIT_ZSTD_LIB (…/zstd/lib), MODKIT_LZ4_LIB (…/lz4/lib), MODKIT_JSON_INCLUDE (folder with
#         nlohmann/json.hpp) and, with BOREALIS, MODKIT_BOREALIS_DIR (the borealis checkout root).
# Can be called before project() (the top-level build needs borealis' toolchain files before project()).

get_filename_component(MODKIT_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(MODKIT_DEPS_DIR "" CACHE PATH
    "folder with zstd/, lz4/, nlohmann/, borealis/ (third_party layout); empty: <repo>/third_party if present, else download pinned archives")

set(_MODKIT_ZSTD_URL "https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz")
set(_MODKIT_ZSTD_SHA "eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3")
set(_MODKIT_LZ4_URL "https://github.com/lz4/lz4/releases/download/v1.10.0/lz4-1.10.0.tar.gz")
set(_MODKIT_LZ4_SHA "537512904744b35e232912055ccf8ec66d768639ff3abe5788d90d792ec5f48b")
set(_MODKIT_JSON_URL "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp")
set(_MODKIT_JSON_SHA "9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6")
set(_MODKIT_BOREALIS_URL "https://github.com/xfangfang/borealis/archive/5f08b286f3df737f3321d2247a6fe633fcead03c.tar.gz")
set(_MODKIT_BOREALIS_SHA "ee7d7c8ba233cee10ba502638db83c4394d37e038119c05b4b0cfeefd765d700")

# _modkit_local(<out> <subfolder> <marker file inside it>) — the local folder, or "" when it must be downloaded.
function(_modkit_local out sub marker)
    if (MODKIT_DEPS_DIR)
        if (NOT EXISTS "${MODKIT_DEPS_DIR}/${sub}/${marker}")
            message(FATAL_ERROR "MODKIT_DEPS_DIR=${MODKIT_DEPS_DIR}: no ${sub}/${marker} there. "
                                "Put it there or leave MODKIT_DEPS_DIR empty to download the pinned archive.")
        endif ()
        set(${out} "${MODKIT_DEPS_DIR}/${sub}" PARENT_SCOPE)
    elseif (EXISTS "${MODKIT_REPO_ROOT}/third_party/${sub}/${marker}")
        set(${out} "${MODKIT_REPO_ROOT}/third_party/${sub}" PARENT_SCOPE)
    else ()
        set(${out} "" PARENT_SCOPE)
    endif ()
endfunction()

# _modkit_fetch(<out> <name> <url> <sha256>) — download + verify + unpack a release archive into <build>/_deps.
# SOURCE_SUBDIR points to a folder that does not exist: the sources are only unpacked, never add_subdirectory'd
# (borealis has its own top-level CMakeLists.txt — the demo; zstd and lz4 are compiled by our lists).
function(_modkit_fetch out name url sha)
    include(FetchContent)
    set(_ts "")
    if (NOT CMAKE_VERSION VERSION_LESS 3.24)
        set(_ts DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    endif ()
    message(STATUS "ModKitNX deps: ${name} <- ${url} (sha256 ${sha})")
    FetchContent_Declare(modkit_${name} URL "${url}" URL_HASH SHA256=${sha} ${_ts} SOURCE_SUBDIR _modkit_sources_only)
    FetchContent_MakeAvailable(modkit_${name})
    set(${out} "${modkit_${name}_SOURCE_DIR}" PARENT_SCOPE)
endfunction()

function(modkit_deps)
    cmake_parse_arguments(ARG "BOREALIS" "" "" ${ARGN})

    _modkit_local(_d zstd lib/zstd.h)
    if (NOT _d)
        _modkit_fetch(_d zstd "${_MODKIT_ZSTD_URL}" "${_MODKIT_ZSTD_SHA}")
    endif ()
    set(MODKIT_ZSTD_LIB "${_d}/lib" PARENT_SCOPE)
    set(_report "zstd=${_d}")

    _modkit_local(_d lz4 lib/lz4.h)
    if (NOT _d)
        _modkit_fetch(_d lz4 "${_MODKIT_LZ4_URL}" "${_MODKIT_LZ4_SHA}")
    endif ()
    set(MODKIT_LZ4_LIB "${_d}/lib" PARENT_SCOPE)
    list(APPEND _report "lz4=${_d}")

    # json is a single header, not an archive: file(DOWNLOAD) with the pinned hash into an include layout.
    _modkit_local(_d nlohmann nlohmann/json.hpp)
    if (NOT _d)
        set(_d "${CMAKE_BINARY_DIR}/_deps/modkit_json-include")
        set(_hpp "${_d}/nlohmann/json.hpp")
        if (EXISTS "${_hpp}")
            file(SHA256 "${_hpp}" _have)
        endif ()
        if (NOT EXISTS "${_hpp}" OR NOT _have STREQUAL _MODKIT_JSON_SHA)
            message(STATUS "ModKitNX deps: json <- ${_MODKIT_JSON_URL} (sha256 ${_MODKIT_JSON_SHA})")
            file(DOWNLOAD "${_MODKIT_JSON_URL}" "${_hpp}.part" EXPECTED_HASH SHA256=${_MODKIT_JSON_SHA}
                 TLS_VERIFY ON STATUS _st)
            list(GET _st 0 _code)
            if (NOT _code EQUAL 0)
                message(FATAL_ERROR "json.hpp download failed: ${_st}")
            endif ()
            file(RENAME "${_hpp}.part" "${_hpp}")
        endif ()
    endif ()
    set(MODKIT_JSON_INCLUDE "${_d}" PARENT_SCOPE)
    list(APPEND _report "json=${_d}")

    if (ARG_BOREALIS)
        _modkit_local(_d borealis library/CMakeLists.txt)
        if (NOT _d)
            _modkit_fetch(_d borealis "${_MODKIT_BOREALIS_URL}" "${_MODKIT_BOREALIS_SHA}")
        endif ()
        set(MODKIT_BOREALIS_DIR "${_d}" PARENT_SCOPE)
        list(APPEND _report "borealis=${_d}")
    endif ()
    list(JOIN _report "; " _report)
    message(STATUS "ModKitNX deps: ${_report}")
endfunction()
