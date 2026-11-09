# ModKitNX — top-level build of the Switch application (.nro).
#
# Why CMake and not a switch_rules Makefile (as TorrentShopNX): borealis (xfangfang fork) is a CMake
# project — commonOption.cmake/toolchain.cmake/program_target, used this way by NX-torrent-player and by
# borealis/demo — so it is built from source here, with no prebuilt libborealis.a whose recipe is not
# in the repository. The host tests of core/ are CMake as well (tests/host/), one build system for both.
#
# Configure/build (devkitPro with switch-cmake on Linux/macOS/WSL, or the devkitpro/devkita64 Docker image):
#   cmake -B build -DPLATFORM_SWITCH=ON -DCMAKE_BUILD_TYPE=Release
#   cmake --build build -j          -> build/ModKitNX.nro
# Host tests are NOT built here: cmake -S tests/host -B build-host.
#
# Name, version, author — project.cmake only (one place). Third-party sources (zstd, lz4, nlohmann/json,
# borealis) — cmake/ModkitDeps.cmake: pinned archives with SHA-256, downloaded into the build folder, or taken
# from -DMODKIT_DEPS_DIR=<folder> / an existing third_party/ (no network then).

cmake_minimum_required(VERSION 3.15)

include(${CMAKE_CURRENT_SOURCE_DIR}/project.cmake)

if (NOT PLATFORM_SWITCH)
    message(FATAL_ERROR
        "This CMakeLists.txt builds the Switch .nro only: configure with -DPLATFORM_SWITCH=ON "
        "Host tests of the core: cmake -S tests/host -B build-host.")
endif ()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/ModkitDeps.cmake)
modkit_deps(BOREALIS)
set(BOREALIS_DIR "" CACHE PATH "borealis checkout to use instead of cmake/ModkitDeps.cmake (empty: pinned commit)")
if (NOT BOREALIS_DIR)
    set(BOREALIS_DIR ${MODKIT_BOREALIS_DIR})
endif ()
set(BOREALIS_LIBRARY ${BOREALIS_DIR}/library)
if (NOT EXISTS ${BOREALIS_LIBRARY}/CMakeLists.txt)
    message(FATAL_ERROR "borealis not found at ${BOREALIS_DIR} (no library/CMakeLists.txt)")
endif ()

# borealis options — as NX-torrent-player / borealis demo. Flags the UI needs are listed in ui/README.md;
# add them here, next to these, so the whole configuration stays in one file.
include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)
option(USE_SHARED_LIB "" OFF)
cmake_dependent_option(USE_SYSTEM_FMT "" OFF "NOT USE_SHARED_LIB" ON)
cmake_dependent_option(USE_SYSTEM_TINYXML2 "" OFF "NOT USE_SHARED_LIB" ON)
cmake_dependent_option(USE_SYSTEM_TWEENY "" OFF "NOT USE_SHARED_LIB" ON)
set(CMAKE_NO_SYSTEM_FROM_IMPORTED ON)
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)

project(${APP_NAME} LANGUAGES C CXX ASM)

# devkitPro platform flags. borealis' toolchain.cmake sets CMAKE_C_FLAGS as a plain variable BEFORE project(),
# which shadows the CMAKE_C_FLAGS_INIT of devkitPro's NintendoSwitch platform — so -mtp=soft and -march were lost.
# Without -mtp=soft the compiler reads TLS straight from tpidr_el0, which libnx leaves 0 in new threads (libnx
# keeps the thread pointer in ThreadVars, read by __aarch64_read_tp): any thread_local in a brls::async thread
# crashed with a Data Abort at 0x18 (opening a ZBIC game). Put them back for every target.
if (NOT NX_ARCH_SETTINGS)
    message(FATAL_ERROR "NX_ARCH_SETTINGS is empty: devkitPro's NintendoSwitch platform file was not loaded")
endif ()
foreach (lang C CXX ASM)
    string(APPEND CMAKE_${lang}_FLAGS " ${NX_ARCH_SETTINGS} ${NX_COMMON_FLAGS}")
    # Paths of the build machine (user name, folders) must not end up in the .nro via __FILE__/assert/debug info.
    string(APPEND CMAKE_${lang}_FLAGS " -ffile-prefix-map=${CMAKE_SOURCE_DIR}=. -ffile-prefix-map=${CMAKE_BINARY_DIR}=build")
endforeach ()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(${BOREALIS_LIBRARY} borealis)

# ---------------------------------------------------------------- core (C++17, no libnx)
# If core/ brings its own CMakeLists.txt with a library target, use it (MODKIT_CORE_TARGET);
# otherwise compile core/src/** directly, minus host-only entry points (*_cli*, *main*).
set(MODKIT_CORE_TARGET "mkcore" CACHE STRING "library target defined by core/CMakeLists.txt")
set(APP_LIBS "")
set(APP_SRC "")
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/core/CMakeLists.txt)
    add_subdirectory(core core)
    if (NOT TARGET ${MODKIT_CORE_TARGET})
        message(FATAL_ERROR "core/CMakeLists.txt does not define target '${MODKIT_CORE_TARGET}' (set -DMODKIT_CORE_TARGET=...)")
    endif ()
    list(APPEND APP_LIBS ${MODKIT_CORE_TARGET})
else ()
    # Fallback until core/CMakeLists.txt exists: the same library as tests/host/CMakeLists.txt (target mkcore).
    # Keep the zstd list identical to tests/host: entropy_common.c is compiled inside core/src/zstd_bic_glue.c,
    # so globbing zstd/lib/common/*.c would define its symbols twice.
    set(ZSTD ${MODKIT_ZSTD_LIB})
    file(GLOB CORE_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/core/src/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/core/src/*.c)
    add_library(mkcore STATIC ${CORE_SRC}
        ${ZSTD}/common/debug.c ${ZSTD}/common/error_private.c ${ZSTD}/common/fse_decompress.c
        ${ZSTD}/common/xxhash.c ${ZSTD}/common/zstd_common.c
        ${ZSTD}/decompress/huf_decompress.c ${ZSTD}/decompress/zstd_ddict.c
        ${ZSTD}/decompress/zstd_decompress.c ${ZSTD}/decompress/zstd_decompress_block.c
        ${MODKIT_LZ4_LIB}/lz4.c)
    target_include_directories(mkcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/include ${MODKIT_JSON_INCLUDE} ${ZSTD} ${MODKIT_LZ4_LIB}
                               PRIVATE ${ZSTD}/common ${CMAKE_CURRENT_SOURCE_DIR}/core/src)
    target_compile_definitions(mkcore PUBLIC ZSTD_DISABLE_ASM=1 PRIVATE ZSTD_LEGACY_SUPPORT=0)
    set_target_properties(mkcore PROPERTIES CXX_STANDARD 17)
    list(APPEND APP_LIBS mkcore)
endif ()

# ---------------------------------------------------------------- platform (libnx), ui (borealis), entry
file(GLOB_RECURSE PLATFORM_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/platform/src/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/platform/src/*.c)
file(GLOB_RECURSE UI_SRC       CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ui/*.cpp)
file(GLOB_RECURSE MAIN_SRC     CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/source/*.c)
list(APPEND APP_SRC ${PLATFORM_SRC} ${UI_SRC} ${MAIN_SRC})
# borealis' own userAppInit/userAppExit. If platform/ defines its own, set -DMODKIT_OWN_APPINIT=ON.
option(MODKIT_OWN_APPINIT "platform/ defines userAppInit/userAppExit itself" OFF)
if (NOT MODKIT_OWN_APPINIT)
    list(APPEND APP_SRC ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c)
endif ()
if (NOT APP_SRC)
    message(FATAL_ERROR "no sources found (core/src, platform/src, ui, source)")
endif ()

program_target(${PROJECT_NAME} "${APP_SRC}")
set_target_properties(${PROJECT_NAME} PROPERTIES CXX_STANDARD 17)

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/core/include
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/include
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/ui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/source
    ${MODKIT_JSON_INCLUDE}
    ${MODKIT_ZSTD_LIB}
    ${MODKIT_LZ4_LIB})

# One source of truth for name/version (project.cmake): NACP, the About screen, the receipt.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    APP_NAME="${APP_NAME}"
    APP_VERSION="${APP_VERSION}"
    APP_AUTHOR="${APP_AUTHOR}"
    BRLS_RESOURCES="romfs:/")

# A printf whose arguments do not match the format is a console crash on Switch: make it an error.
target_compile_options(${PROJECT_NAME} PRIVATE -Wformat -Werror=format -ffunction-sections -fdata-sections ${APP_PLATFORM_OPTION})
target_link_options(${PROJECT_NAME} PRIVATE -Wl,--gc-sections)

find_package(PkgConfig REQUIRED)
set(PKG_CONFIG_USE_STATIC_LIBS ON)
pkg_check_modules(ZLIB_PC REQUIRED IMPORTED_TARGET zlib)   # NACP title table DEFLATE (SDK 20+)
# App update from GitHub releases (R-24): curl from devkitPro portlibs, TLS through the console's ssl service.
pkg_check_modules(CURL_PC REQUIRED IMPORTED_TARGET libcurl)
target_compile_definitions(${PROJECT_NAME} PRIVATE MK_HAVE_CURL=1)
target_link_libraries(${PROJECT_NAME} PRIVATE borealis PkgConfig::CURL_PC PkgConfig::ZLIB_PC ${APP_LIBS} ${APP_PLATFORM_LIB})

# ---------------------------------------------------------------- romfs + NACP + NRO
# romfs = borealis resources (minus borealis' bundled font: it is Nintendo's, not ours to ship)
#       + resources/ (our XML, i18n, images, UI font font/font.ttf)  + romfs/ (nxredirect modules, built from nxredirect/ by nxredirect/build-romfs-modules.sh).
set(ROMFS_STAGE ${CMAKE_BINARY_DIR}/romfs)
set(APP_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon.jpg)
if (NOT EXISTS ${APP_ICON})
    set(APP_ICON $ENV{DEVKITPRO}/libnx/default_icon.jpg)
endif ()
# Only these borealis resources are copied: i18n (button hints; the folder also carries borealis' own small demo
# strings), system icons, the Material icon font — no bundled borealis font.
# UI font: resources/font/font.ttf (Noto Sans subset, SIL OFL 1.1, see resources/CREDITS.md) -> romfs:/font/font.ttf.
# Borealis on Switch uses it as the main font of all text (switch_font.cpp, USER_FONT_PATH). Without it the
# console's Chinese shared font draws Cyrillic with full-width glyphs.
if (NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/resources/font/font.ttf)
    message(FATAL_ERROR "resources/font/font.ttf (UI font, see resources/CREDITS.md) is missing")
endif ()
# The stage is rebuilt from scratch on every build: a file removed from resources/ or romfs/ must not live on in the
# .nro (it did: an old reference icon with a Nintendo logo stayed in the stage after it left git). deko3d shaders are generated at configure time into their own folder and copied in.
set(ROMFS_SHADERS ${CMAKE_BINARY_DIR}/romfs-shaders)
set(ROMFS_COMMANDS
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${ROMFS_STAGE}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ROMFS_STAGE}
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${BOREALIS_DIR}/resources/i18n ${ROMFS_STAGE}/i18n
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${BOREALIS_DIR}/resources/img/sys ${ROMFS_STAGE}/img/sys
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${BOREALIS_DIR}/resources/material ${ROMFS_STAGE}/material)
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/resources)
    list(APPEND ROMFS_COMMANDS COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/resources ${ROMFS_STAGE})
endif ()
if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/romfs)
    list(APPEND ROMFS_COMMANDS COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/romfs ${ROMFS_STAGE})
endif ()
# Renderer: OpenGL (mesa + glfw; default, as NX-torrent-player) or deko3d (-DUSE_DEKO3D=ON, needs only the
# deko3d and uam packages). deko3d reads its shaders from romfs:/shaders/*.dksh — compiled here by uam at
# configure time, as borealis/CMakeLists.txt (demo) does.
if (USE_DEKO3D)
    gen_dksh("${ROMFS_SHADERS}/shaders")
    list(APPEND ROMFS_COMMANDS COMMAND ${CMAKE_COMMAND} -E copy_directory ${ROMFS_SHADERS} ${ROMFS_STAGE})
endif ()
add_custom_target(${PROJECT_NAME}.nro ALL DEPENDS ${PROJECT_NAME}
    ${ROMFS_COMMANDS}
    COMMAND ${NX_NACPTOOL_EXE} --create "${APP_NAME}" "${APP_AUTHOR}" "${APP_VERSION}" ${PROJECT_NAME}.nacp
    COMMAND ${NX_ELF2NRO_EXE} ${PROJECT_NAME}.elf ${PROJECT_NAME}.nro --icon=${APP_ICON} --nacp=${PROJECT_NAME}.nacp --romfsdir=${ROMFS_STAGE}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Packing ${PROJECT_NAME}.nro (version ${APP_VERSION})")
