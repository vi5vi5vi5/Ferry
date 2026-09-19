# ============================================================
#  Ferry — паром для файлов.
#
#  Корневой проект собирает общий код и CLI-клиент. Сервер собирается
#  отдельно (Server/CMakeLists.txt), потому что ему нужен Qt6, а клиенту
#  и ядру — нет, и тащить Qt в сборку клиента не за чем.
#
#  Локально:
#    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
#  В докере это же делает Server/Dockerfile отдельной стадией.
# ============================================================
cmake_minimum_required(VERSION 3.16)

project(Ferry LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(NOT MSVC)
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

option(FERRY_BUILD_CLI "Собирать CLI-клиент" ON)
option(FERRY_BUILD_TESTS "Собирать самотесты ядра" ON)

# Статический OpenSSL — для бинаря, который скачивают и запускают на чужой
# машине: soname libssl отличается между дистрибутивами (1.1 против 3), и
# динамическая сборка просто не запустится там, где версия другая.
#
# Флаг обязан стоять ДО первого find_package(OpenSSL): FindOpenSSL читает
# OPENSSL_USE_STATIC_LIBS и подставляет в импортируемые цели .a вместо .so.
# Дописать «-l:libssl.a» к уже готовой цели нельзя — компоновщик возьмёт
# то, что стоит в строке раньше, то есть динамическую библиотеку, и
# статическая просто не пригодится. На это легко потратить час.
option(FERRY_STATIC_OPENSSL "Влинковать OpenSSL статически (для раздаваемого бинаря)" OFF)
if(FERRY_STATIC_OPENSSL)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()

add_subdirectory(core)

if(FERRY_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(FERRY_BUILD_CLI)
    add_subdirectory(Cli)
endif()
