# ModKitNX — хост-тесты ядра (ПК): библиотека core, core_cli, юнит-тесты.
# Зависимости — cmake/ModkitDeps.cmake: закреплённые архивы с SHA-256 (скачиваются в папку сборки) или уже лежащие
# в third_party/ / -DMODKIT_DEPS_DIR=<папка> (тогда без сети).
#   cmake -S tests/host -B build-host && cmake --build build-host -j && ctest --test-dir build-host
cmake_minimum_required(VERSION 3.16)
project(modkit_core_host C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
include("${REPO}/cmake/ModkitDeps.cmake")
modkit_deps()
set(ZSTD "${MODKIT_ZSTD_LIB}")
set(LZ4 "${MODKIT_LZ4_LIB}")
set(JSON "${MODKIT_JSON_INCLUDE}")

# --- upstream zstd: только распаковка; entropy_common.c входит в core/src/zstd_bic_glue.c
set(ZSTD_SRC
  ${ZSTD}/common/debug.c ${ZSTD}/common/error_private.c ${ZSTD}/common/fse_decompress.c
  ${ZSTD}/common/xxhash.c ${ZSTD}/common/zstd_common.c
  ${ZSTD}/decompress/huf_decompress.c ${ZSTD}/decompress/zstd_ddict.c
  ${ZSTD}/decompress/zstd_decompress.c ${ZSTD}/decompress/zstd_decompress_block.c)

set(CORE "${REPO}/core")
add_library(mkcore STATIC
  ${CORE}/src/core.cpp ${CORE}/src/nso.cpp ${CORE}/src/zbic.cpp ${CORE}/src/zstd_bic_glue.c
  ${CORE}/src/npdm.cpp ${CORE}/src/receipt.cpp ${CORE}/src/plan.cpp ${CORE}/src/mod.cpp
  ${CORE}/src/install.cpp ${CORE}/src/sdk.cpp ${CORE}/src/nacp.cpp ${CORE}/src/logs.cpp ${CORE}/src/sdk_harvest.cpp
  ${CORE}/src/aes128.cpp ${CORE}/src/nca.cpp
  ${ZSTD_SRC} ${LZ4}/lz4.c)
target_include_directories(mkcore PUBLIC ${CORE}/include ${JSON} ${ZSTD} ${LZ4}
                           PRIVATE ${ZSTD}/common ${CORE}/src)
target_compile_definitions(mkcore PUBLIC ZSTD_DISABLE_ASM=1 PRIVATE ZSTD_LEGACY_SUPPORT=0)
target_compile_options(mkcore PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wno-unused-parameter>)
# каталог автоисправления: по умолчанию копия в core/src; NXR_CATALOG — взять файл эталона (nxredirect/src)
if(NXR_CATALOG)
  target_compile_definitions(mkcore PRIVATE MK_CATALOG_PATH="${NXR_CATALOG}")
endif()

add_library(mkhost STATIC host_fs.cpp)
target_link_libraries(mkhost PUBLIC mkcore)

add_executable(core_cli core_cli.cpp)
target_link_libraries(core_cli PRIVATE mkhost)

add_executable(unit_tests unit_tests.cpp)
target_link_libraries(unit_tests PRIVATE mkhost)

# Чтение exefs из сырого NCA (mk::nca, mk::aes): синтетический NCA с выдуманными ключами.
add_executable(nca_tests nca_tests.cpp)
target_link_libraries(nca_tests PRIVATE mkcore)

enable_testing()
add_test(NAME unit COMMAND unit_tests)
add_test(NAME nca COMMAND nca_tests)

# Обновление приложения и настройки (переносимая часть platform/: app_update, app_config) — подставные ответы GitHub
# и собранные в памяти .nro, без сети (app_update_tests.cpp).
include("${REPO}/project.cmake")
add_executable(app_update_tests app_update_tests.cpp
  ${REPO}/platform/src/app_update.cpp ${REPO}/platform/src/app_config.cpp
  ${REPO}/platform/src/log.cpp ${REPO}/platform/src/paths.cpp)
target_include_directories(app_update_tests PRIVATE ${REPO}/platform/include)
target_compile_definitions(app_update_tests PRIVATE APP_NAME="${APP_NAME}" APP_VERSION="${APP_VERSION}")
target_compile_options(app_update_tests PRIVATE -Wall -Wextra -Wformat -Werror=format)
target_link_libraries(app_update_tests PRIVATE mkcore)
add_test(NAME app_update COMMAND app_update_tests)
