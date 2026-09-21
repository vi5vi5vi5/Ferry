# ============================================================
#  ferry — CLI-клиент. Один бинарь, ноль установки.
#
#  Зависимости: ядро Ferry, статически влинкованный OpenSSL и вендоренный
#  генератор QR. Qt здесь нет и не будет: клиент должен ставиться одним
#  файлом на машину, куда человек зашёл по ssh.
# ============================================================

if(NOT TARGET ferry-crypto)
    message(FATAL_ERROR
        "Для сборки клиента нужен OpenSSL (пакет libssl-dev). "
        "Собрать без клиента: cmake -DFERRY_BUILD_CLI=OFF")
endif()

# QR: официальная реализация Nayuki, один файл на C.
add_library(ferry-qrcodegen STATIC third_party/qrcodegen/qrcodegen.c)
target_include_directories(ferry-qrcodegen PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/qrcodegen)

add_executable(ferry
    main.cpp
    ChunkPump.h   ChunkPump.cpp
    Tree.h        Tree.cpp
    VolumeFile.h  VolumeFile.cpp
    Uninstall.h   Uninstall.cpp
    Options.h     Options.cpp
    Relay.h       Relay.cpp
    Sender.h      Sender.cpp
    Receiver.h    Receiver.cpp
    Signals.h     Signals.cpp

    platform/Platform.h
    platform/Platform_posix.cpp
    platform/Platform_windows.cpp

    net/TlsSocket.h        net/TlsSocket.cpp
    net/HttpClient.h       net/HttpClient.cpp
    net/WebSocketClient.h  net/WebSocketClient.cpp

    ui/Term.h       ui/Term.cpp
    ui/LivePanel.h  ui/LivePanel.cpp
    ui/Qr.h         ui/Qr.cpp
)

# Инклуды от корня репозитория: #include "Cli/net/TlsSocket.h" и
# #include "core/Chunker.h" — одинаково для своего кода и для общего.
target_include_directories(ferry PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(ferry PRIVATE ferry-core ferry-crypto ferry-qrcodegen)

# Windows: сокеты (ws2_32), разбор командной строки в UTF-16 (shell32) и
# системное хранилище сертификатов (crypt32) — без последнего статический
# OpenSSL не находит ни одного корня и не верит ни одному сертификату.
# Все три библиотеки системные, ставить ничего не нужно.
if(WIN32)
    target_link_libraries(ferry PRIVATE ws2_32 shell32 crypt32)
endif()

# TLS ещё и в самом клиенте (рукопожатие WebSocket) — помимо криптографии
# тома, которая живёт в ferry-crypto.
find_package(OpenSSL REQUIRED)
target_link_libraries(ferry PRIVATE OpenSSL::SSL OpenSSL::Crypto)

set(FERRY_COMMIT "unknown" CACHE STRING "Коммит, из которого собран клиент")
string(TIMESTAMP FERRY_BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)
target_compile_definitions(ferry PRIVATE
    FERRY_COMMIT="${FERRY_COMMIT}"
    FERRY_BUILD_TIME="${FERRY_BUILD_TIME}"
)

# Статическая сборка. Нужна ровно для одного: чтобы скачанный бинарь
# запускался на чужой машине без установки чего бы то ни было.
#
# libstdc++ и libgcc — всегда: версия ABI разная между дистрибутивами.
# OpenSSL — по флагу FERRY_STATIC_OPENSSL, который включается в докере
# (см. корневой CMakeLists и Server/Dockerfile). При локальной сборке для
# себя статический OpenSSL, наоборот, вреден: обновление системного
# пакета безопасности до бинаря уже не дойдёт.
if(NOT MSVC)
    target_link_options(ferry PRIVATE -static-libstdc++ -static-libgcc)
endif()

if(FERRY_STATIC_OPENSSL AND NOT MSVC)
    # Архивам OpenSSL нужны dl и pthread — динамическая версия тянула бы
    # их сама.
    target_link_libraries(ferry PRIVATE ${CMAKE_DL_LIBS} pthread)
endif()
