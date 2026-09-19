# ============================================================
#  Ferry — сервер-релей. C++20 + Qt6 (Core, Network, WebSockets).
#
#  Отдельный проект, а не часть корневого: серверу нужен Qt, клиенту и ядру
#  — нет, и тащить Qt в сборку клиента незачем. Общий код подключается
#  подкаталогом из ../core.
#
#  Заметьте, чего здесь НЕТ: ни SQLite, ни OpenSSL. Базы у Ferry нет
#  сознательно (§9), а криптография серверу не нужна вовсе — он никогда
#  ничего не расшифровывает (§10). Если в этом файле однажды появится
#  ferry-crypto, значит в серверный код заехало то, чего там быть не должно.
# ============================================================
cmake_minimum_required(VERSION 3.16)

project(FerryServer LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Network WebSockets)

# Общее ядро. FERRY_BUILD_CLI/TESTS не трогаем — здесь подключается только
# сам каталог core, без корневого проекта.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_CURRENT_BINARY_DIR}/core)

add_executable(FerryServer
    main.cpp

    # config/ — настройки (<config-dir>/ferry.conf) и правила журнала
    config/ServerConfig.h      config/ServerConfig.cpp
    config/Log.h               config/Log.cpp

    # core/ — модель времени выполнения: раздача, окно, реестр, challenge
    core/RingWindow.h          core/RingWindow.cpp
    core/TransferSession.h     core/TransferSession.cpp
    core/TransferRegistry.h    core/TransferRegistry.cpp
    core/ChallengeStore.h      core/ChallengeStore.cpp

    # network/ — входные точки: WebSocket-релей и HTTP
    network/ClientSession.h    network/ClientSession.cpp
    network/TransferServer.h   network/TransferServer.cpp
    network/HttpRequest.h      network/HttpRequest.cpp
    network/HttpApi.h          network/HttpApi.cpp
    network/HttpFileServer.h   network/HttpFileServer.cpp
)

# Инклуды пишутся от корня Server/: #include "core/RingWindow.h".
# Общий код — от корня репозитория: #include "core/Chunker.h" даёт ../core.
# Совпадение имён здесь не случайно и не мешает: ferry-core экспортирует
# путь на уровень выше, поэтому "core/Chunker.h" находится в ../core, а
# "core/RingWindow.h" — здесь. Разводит их то, что файлы не пересекаются.
target_include_directories(FerryServer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(FerryServer PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::WebSockets
    ferry-core
)

# Путь к страницам по умолчанию — папка рядом с исходниками.
target_compile_definitions(FerryServer PRIVATE
    FERRY_WEB_ROOT_DEFAULT="${CMAKE_CURRENT_SOURCE_DIR}/web"
)

# Из какого коммита собран сервер — он отдаёт это в GET /api/health.
# Значения приходят снаружи, а не считаются здесь: в докере .git в контекст
# сборки не попадает. Собрали руками без -D — получится "unknown", и это
# честный ответ, а не ошибка.
set(FERRY_COMMIT "unknown" CACHE STRING "Коммит, из которого собран сервер")
set(FERRY_MODIFIED "0"     CACHE STRING "1, если в рабочей копии были правки")

string(TIMESTAMP FERRY_BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)

target_compile_definitions(FerryServer PRIVATE
    FERRY_COMMIT="${FERRY_COMMIT}"
    FERRY_MODIFIED=${FERRY_MODIFIED}
    FERRY_BUILD_TIME="${FERRY_BUILD_TIME}"
)
