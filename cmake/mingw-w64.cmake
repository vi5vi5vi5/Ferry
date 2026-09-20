# ============================================================
#  Кросс-компиляция клиента под Windows из Linux (MinGW-w64).
#
#  Зачем вообще кросс-компиляция: релей раздаёт собственный клиент, и
#  ferry.exe должен собираться там же, где собирается сервер — в том же
#  образе, одной командой `./tools/update.sh`. Иначе понадобилась бы
#  вторая машина с Visual Studio и ручная выкладка релизов, то есть ровно
#  то, чего мы избегали в линуксовой ветке.
#
#  Использование:
#    cmake -S . -B build-win \
#        -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
#        -DFERRY_WIN_DEPS=/opt/openssl-win
#
#  FERRY_WIN_DEPS — куда установлен OpenSSL, собранный для MinGW
#  (см. Server/Dockerfile, стадия build-client-windows).
# ============================================================

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(FERRY_MINGW_PREFIX x86_64-w64-mingw32)

set(CMAKE_C_COMPILER   ${FERRY_MINGW_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${FERRY_MINGW_PREFIX}-g++)
set(CMAKE_RC_COMPILER  ${FERRY_MINGW_PREFIX}-windres)

# Где искать библиотеки и заголовки цели. Программы (компиляторы, perl)
# при этом берём с хоста — отсюда NEVER для PROGRAM.
set(CMAKE_FIND_ROOT_PATH /usr/${FERRY_MINGW_PREFIX} ${FERRY_WIN_DEPS})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

if(FERRY_WIN_DEPS)
    set(OPENSSL_ROOT_DIR ${FERRY_WIN_DEPS})
    list(APPEND CMAKE_PREFIX_PATH ${FERRY_WIN_DEPS})
endif()

# Windows 10 и новее. Нужно именно тут, а не в коде: Windows-заголовки
# смотрят на _WIN32_WINNT ещё до того, как до них доберётся наш #include,
# и без этого часть нужных объявлений (например, флаги консоли) просто не
# появится.
add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00)

# Стандартный printf вместо msvcrt-шного.
#
# Это не косметика. Рантайм msvcrt не знает ни %zu, ни %lld — а у нас ими
# печатаются размеры томов и позиции в разборе JSON. Без этой строки
# «5 368 709 120 байт» превратилось бы в мусор вроде «5368709120d», и
# заметили бы мы это в лучшем случае на глаз.
add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
