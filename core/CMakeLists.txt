# ============================================================
#  libferry-core — общий код Ferry.
#
#  Две цели, и разделение между ними принципиальное:
#
#    ferry-core    чанкование, манифест, ссылка, BLAKE3, JSON.
#                  Ноль внешних зависимостей. Линкуется в сервер.
#
#    ferry-crypto  HKDF, HMAC, AES-256-GCM. Требует OpenSSL.
#                  Линкуется ТОЛЬКО в клиент.
#
#  Сервер никогда ничего не расшифровывает (§10 проектного документа), и
#  это должно держаться само. Если однажды ferry-crypto понадобится
#  серверу — значит в серверный код заехало то, чего там быть не должно.
# ============================================================
cmake_minimum_required(VERSION 3.16)

# ---------- BLAKE3 ----------
# Официальная C-реализация (BLAKE3-team/BLAKE3, c/). Берём варианты на
# интринсиках, а не на ассемблере: так собирается любым gcc/clang без
# ассемблера в образе, а диспетчер всё равно выбирает лучший вариант в
# рантайме. Флаги на каждый файл обязательны — без -mavx2 компилятор не
# знает соответствующих интринсиков.
set(FERRY_BLAKE3_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/blake3)

set(FERRY_BLAKE3_SRC
    ${FERRY_BLAKE3_DIR}/blake3.c
    ${FERRY_BLAKE3_DIR}/blake3_dispatch.c
    ${FERRY_BLAKE3_DIR}/blake3_portable.c
)

set(FERRY_BLAKE3_DEFS "")

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    list(APPEND FERRY_BLAKE3_SRC
        ${FERRY_BLAKE3_DIR}/blake3_sse2.c
        ${FERRY_BLAKE3_DIR}/blake3_sse41.c
        ${FERRY_BLAKE3_DIR}/blake3_avx2.c
        ${FERRY_BLAKE3_DIR}/blake3_avx512.c
    )
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    list(APPEND FERRY_BLAKE3_SRC ${FERRY_BLAKE3_DIR}/blake3_neon.c)
    list(APPEND FERRY_BLAKE3_DEFS BLAKE3_USE_NEON=1)
else()
    # Незнакомая архитектура — только переносимая реализация. Она заметно
    # медленнее, но всё ещё быстрее нашего сетевого канала.
    list(APPEND FERRY_BLAKE3_DEFS
        BLAKE3_NO_SSE2 BLAKE3_NO_SSE41 BLAKE3_NO_AVX2 BLAKE3_NO_AVX512)
endif()

# AUTOMOC может быть включён родительским проектом (сервер на Qt). В ядре
# нет ни одного Q_OBJECT, и лишний проход moc здесь только тратит время.
set(CMAKE_AUTOMOC OFF)

add_library(ferry-blake3 STATIC ${FERRY_BLAKE3_SRC})
target_include_directories(ferry-blake3 PUBLIC ${FERRY_BLAKE3_DIR})
target_compile_definitions(ferry-blake3 PUBLIC ${FERRY_BLAKE3_DEFS})
set_target_properties(ferry-blake3 PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" AND NOT MSVC)
    set_source_files_properties(${FERRY_BLAKE3_DIR}/blake3_sse2.c
        PROPERTIES COMPILE_OPTIONS "-msse2")
    set_source_files_properties(${FERRY_BLAKE3_DIR}/blake3_sse41.c
        PROPERTIES COMPILE_OPTIONS "-msse4.1")
    set_source_files_properties(${FERRY_BLAKE3_DIR}/blake3_avx2.c
        PROPERTIES COMPILE_OPTIONS "-mavx2")
    set_source_files_properties(${FERRY_BLAKE3_DIR}/blake3_avx512.c
        PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512vl")
endif()

# ---------- ferry-core ----------
add_library(ferry-core STATIC
    Base64Url.h   Base64Url.cpp
    Chunker.h     Chunker.cpp
    ChunkSet.h    ChunkSet.cpp
    HashList.h    HashList.cpp
    Json.h        Json.cpp
    Link.h        Link.cpp
    Manifest.h    Manifest.cpp
    Protocol.h
    Types.h
    VolumeLayout.h VolumeLayout.cpp
)

# Инклуды пишутся от корня репозитория: #include "core/Chunker.h".
target_include_directories(ferry-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(ferry-core PUBLIC ferry-blake3)
set_target_properties(ferry-core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------- ferry-crypto ----------
# Отдельной целью и только для клиента — см. шапку файла.
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    add_library(ferry-crypto STATIC Crypto.h Crypto.cpp)
    target_link_libraries(ferry-crypto PUBLIC ferry-core OpenSSL::Crypto)
    set_target_properties(ferry-crypto PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # HMAC() объявлен устаревшим в OpenSSL 3.0, но работает и там, и в 1.1.1
    # (на котором собирается клиент, см. Server/Dockerfile). Разводить сборку
    # на две ветки ради одной функции дороже, чем снять предупреждение.
    if(NOT MSVC)
        target_compile_options(ferry-crypto PRIVATE -Wno-deprecated-declarations)
    endif()
endif()
