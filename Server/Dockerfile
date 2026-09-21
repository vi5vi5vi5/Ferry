# syntax=docker/dockerfile:1.6
# ============================================================
#  Ferry — релей больших файлов.
#
#  Контекст сборки — КОРЕНЬ репозитория (см. docker-compose.yml): образ
#  собирает и сервер, и общее ядро, и оба CLI-клиента.
#
#  Стадии:
#    build-server          сервер на Qt6 (debian bookworm, пакетный Qt)
#    build-client          клиент под Linux (ubuntu 22.04 — см. почему ниже)
#    build-client-windows  клиент под Windows (кросс-компиляция MinGW-w64)
#    runtime               slim-образ: сервер, страницы и оба бинаря рядом
#
#  ОЧЕРЕДЬ СТАДИЙ. По умолчанию BuildKit запускает независимые стадии
#  ПАРАЛЛЕЛЬНО — три компилятора разом. На машине с двумя потоками и двумя
#  гигабайтами это заканчивается OOM-killer'ом посреди сборки. Поэтому
#  стадии выстроены в цепочку: сервер → клиент Linux → клиент Windows.
#
#  Сделано маркером `/stage-done`, который каждая стадия кладёт после
#  себя, а следующая копирует к себе первой же строкой. Копирование и
#  создаёт ребро в графе: BuildKit не начнёт следующую, пока предыдущая
#  не закончится.
#
#  Содержимое маркера ПОСТОЯННО («ok»), и это не мелочь. Кэш COPY
#  считается по содержимому копируемого, а не по тому, пересобиралась ли
#  стадия-источник. Постоянный маркер значит, что правка в Cli/ не тащит
#  за собой пересборку OpenSSL под MinGW, которая идёт четверть часа.
#  Положи сюда что-нибудь меняющееся — например сам бинарь сервера — и
#  каждое обновление начиналось бы со сборки OpenSSL заново.
#
#  Зачем клиенты собираются внутри образа сервера. Затем, что релей раздаёт
#  собственный клиент: `curl -fsSL https://<релей>/install.sh | sh`. Это
#  делает установку такой же простой, как в MeetUp (где клиент — браузер),
#  и гарантирует, что версия клиента совпадает с версией сервера: протокол
#  не может разъехаться по определению.
# ============================================================

# Какую стадию брать для Windows-клиента: настоящую или заглушку
# (./tools/update.sh --skip-windows).
#
# Объявлено ДО первого FROM намеренно: имя стадии в `COPY --from=` docker
# подставляет на разборе файла, и аргумент, объявленный внутри стадии,
# туда ещё не виден — сборка падает с «invalid reference format».
ARG WINDOWS_STAGE=build-client-windows

# ---------- 1. Сервер ----------
FROM debian:bookworm AS build-server

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        qt6-base-dev \
        qt6-websockets-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Из какого коммита собираемся. Гита внутри образа нет и быть не должно:
# .git в контекст сборки не попадает (.dockerignore). Значения считает
# хост и передаёт сюда — см. tools/update.sh и docker-compose.yml.
ARG GIT_COMMIT=unknown
ARG GIT_MODIFIED=0

# Сколько компиляций разом. Пусто — сколько решит ninja, а он на двух
# ядрах берёт три задачи. На маленькой машине задаётся снаружи:
#   ./tools/update.sh --jobs 1
ARG BUILD_JOBS=

RUN cmake -S Server -B build-server -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DFERRY_COMMIT="${GIT_COMMIT}" -DFERRY_MODIFIED="${GIT_MODIFIED}" \
 && cmake --build build-server ${BUILD_JOBS:+-j ${BUILD_JOBS}}

# Конец стадии — см. «ОЧЕРЕДЬ СТАДИЙ» в шапке.
RUN echo ok > /stage-done

# ---------- 2. Клиент под Linux ----------
# Ubuntu 22.04, а не bookworm, и это не случайность.
#
# Бинарь, собранный на bookworm, требует glibc 2.36 и НЕ ЗАПУСТИТСЯ на
# Ubuntu 22.04 (glibc 2.35) — то есть ровно на той системе, куда его чаще
# всего и ставят. Правило простое: бинарь для раздачи собирается на самой
# СТАРОЙ системе, которую мы готовы поддерживать, а не на самой удобной.
#
# Почему именно 22.04, а не что-нибудь ещё старее. Debian 11 (glibc 2.31)
# дотянулся бы до Ubuntu 20.04, но он снят с поддержки, его пакеты уехали
# в archive.debian.org, и сборка на нём ломается на ровном месте — и,
# что хуже, перестаёт получать исправления в OpenSSL, который мы линкуем
# статически. 22.04 поддерживается до 2027 года и покрывает Ubuntu 22.04+,
# Debian 12+ и всё, что новее.
FROM ubuntu:22.04 AS build-client

# noninteractive: иначе tzdata на ubuntu останавливает сборку вопросом
# про часовой пояс и ждёт ответа до конца времён.
ENV DEBIAN_FRONTEND=noninteractive

# Ждём, пока закончится серверная стадия.
COPY --from=build-server /stage-done /stage-order/server

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        libssl-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

ARG GIT_COMMIT=unknown

# OpenSSL влинковывается статически: снаружи у бинаря остаётся только
# glibc. Самотесты ядра собираются заодно и тут же прогоняются — если
# BLAKE3 или AEAD разошлись с эталоном, образ не соберётся, и это
# правильное место, чтобы об этом узнать.
ARG BUILD_JOBS=

RUN cmake -S . -B build-client -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DFERRY_STATIC_OPENSSL=ON -DFERRY_COMMIT="${GIT_COMMIT}" \
 && cmake --build build-client ${BUILD_JOBS:+-j ${BUILD_JOBS}} \
 && ./build-client/tests/ferry-core-test

# Раскладываем то, что будет раздаваться: бинарь и контрольная сумма.
RUN mkdir -p /out/dl \
 && cp build-client/Cli/ferry "/out/dl/ferry-linux-$(uname -m)" \
 && cd /out/dl && sha256sum ferry-linux-* > SHA256SUMS \
 && cat SHA256SUMS

RUN echo ok > /stage-done

# ---------- 3. Клиент под Windows ----------
# Кросс-компиляция MinGW-w64 прямо здесь, а не на отдельной машине с
# Visual Studio: релей обязан раздавать и ferry.exe тоже, а держать ради
# этого вторую сборочную машину и ручную выкладку релизов — это ровно то,
# чего мы избегали в линуксовой ветке.
#
# Стадия тяжёлая: здесь собирается OpenSSL. Зато собирается ОДИН раз —
# дальше её держит кэш docker, и обычное обновление сервера её не трогает.
# Если ferry.exe не нужен совсем, есть ./tools/update.sh --skip-windows:
# тогда BuildKit эту стадию даже не запустит.
FROM debian:bookworm AS build-client-windows

# Ждём линуксового клиента. Ворота стоят ДО apt и сборки OpenSSL,
# а не перед cmake: иначе самая тяжёлая часть стадии всё равно шла бы
# вперегонки с остальными.
COPY --from=build-client /stage-done /stage-order/client

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        ninja-build \
        mingw-w64 \
        perl \
        curl \
        ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# OpenSSL для MinGW собирается из исходников: готового пакета под эту цель
# в Debian нет. Версия вынесена в аргумент — обновление безопасности
# берётся её сменой и пересборкой, больше ничего делать не нужно.
#
# ТАРБОЛЛ, А НЕ git clone. Клон даже с --depth 1 разворачивает 192 МБ
# рабочего дерева против 53 МБ архива. На медленном канале это
# превращается в четверть часа ожидания на ровном месте — проверено на
# живом сервере, где этот шаг занял 841 секунду.
#
# Сначала GitHub, при неудаче openssl.org: зеркала бывают доступны
# по-разному, а останавливать всю сборку из-за одного недоступного
# хоста незачем.
ARG OPENSSL_VERSION=3.5.8
RUN set -eu; \
    gh="https://github.com/openssl/openssl/releases/download/openssl-${OPENSSL_VERSION}/openssl-${OPENSSL_VERSION}.tar.gz"; \
    org="https://www.openssl.org/source/openssl-${OPENSSL_VERSION}.tar.gz"; \
    curl -fsSL --retry 3 --retry-delay 2 -o /tmp/openssl.tar.gz "$gh" \
      || curl -fsSL --retry 3 --retry-delay 2 -o /tmp/openssl.tar.gz "$org"; \
    mkdir -p /tmp/openssl; \
    tar -xzf /tmp/openssl.tar.gz -C /tmp/openssl --strip-components=1; \
    rm /tmp/openssl.tar.gz

# no-shared: у клиента не должно быть рядом лежащих DLL, весь смысл в том,
#   что это один файл.
# no-apps: утилита командной строки openssl нам не нужна, а линкуется она
#   статически и долго.
# no-tests, no-docs: туда же.
# Ассемблер НЕ отключаем: без него AES-GCM теряет AES-NI и становится в
#   разы медленнее, а мы им шифруем весь поток.
# install_dev вместо install_sw: ставим только заголовки и библиотеки,
#   больше нам от OpenSSL ничего не нужно.
RUN set -eu; \
    cd /tmp/openssl; \
    ./Configure mingw64 no-shared no-tests no-docs no-apps \
        --prefix=/opt/openssl-win --openssldir=/opt/openssl-win/ssl \
        --cross-compile-prefix=x86_64-w64-mingw32-; \
    make -j"$(nproc)"; \
    make install_dev; \
    cd /; rm -rf /tmp/openssl

WORKDIR /src
COPY . .

ARG GIT_COMMIT=unknown

# BUILD_JOBS объявлен здесь, а не выше, и это нарочно: объяви его до
# сборки OpenSSL — и смена --jobs уронила бы её кэш на четверть часа.
# OpenSSL остаётся на nproc сознательно: это простой С, он не ест память
# так, как шаблоны C++, и теперь он в любом случае идёт один, без соседей.
ARG BUILD_JOBS=

RUN cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake \
        -DFERRY_WIN_DEPS=/opt/openssl-win \
        -DFERRY_STATIC_OPENSSL=ON \
        -DFERRY_BUILD_TESTS=OFF \
        -DFERRY_COMMIT="${GIT_COMMIT}" \
 && cmake --build build-win ${BUILD_JOBS:+-j ${BUILD_JOBS}}

RUN mkdir -p /out/dl \
 && cp build-win/Cli/ferry.exe /out/dl/ferry-windows-x86_64.exe \
 && cd /out/dl && sha256sum ferry-windows-x86_64.exe > SHA256SUMS.windows \
 && cat SHA256SUMS.windows

# Заглушка для --skip-windows.
FROM debian:bookworm-slim AS build-client-windows-skip
RUN mkdir -p /out/dl && : > /out/dl/SHA256SUMS.windows

# Выбор между настоящей стадией и заглушкой.
#
# Именно так, через FROM с подстановкой, а не через COPY --from=${ARG}:
# имя стадии в COPY docker разбирает раньше, чем подставляет аргументы, и
# сборка падает с «invalid reference format». А FROM аргумент понимает.
#
# Побочный и главный эффект: BuildKit собирает только те стадии, от
# которых зависит цель. С заглушкой тяжёлая кросс-сборка не запускается
# вообще, а не собирается впустую.
FROM ${WINDOWS_STAGE} AS windows-artifacts

# ---------- 4. Рантайм ----------
FROM debian:bookworm-slim AS runtime

# Только рантайм-библиотеки Qt6. libqt6websockets6 тянет за собой Network
# и Core. Сервер headless (QCoreApplication) — GUI-плагины не нужны.
#
# Заметьте, чего здесь НЕТ: libssl. Серверу криптография не нужна вовсе —
# он никогда ничего не расшифровывает (§10 проектного документа). Если
# однажды образ перестанет запускаться из-за отсутствия libssl, это будет
# означать, что в серверный код заехало то, чего там быть не должно.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        libqt6websockets6 \
        libqt6network6 \
        libqt6core6 \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build-server /src/build-server/FerryServer /app/FerryServer
COPY Server/web /app/web
COPY --from=build-client /out/dl /app/web/dl

COPY --from=windows-artifacts /out/dl /app/web/dl

# Общий список контрольных сумм. Установщики (install.sh и install.ps1)
# читают один и тот же файл и сверяют по нему то, что скачали: бинарь,
# который человек сейчас запустит, — не то место, где уместно «ну и ладно».
RUN cat /app/web/dl/SHA256SUMS.windows >> /app/web/dl/SHA256SUMS \
 && rm /app/web/dl/SHA256SUMS.windows \
 && cat /app/web/dl/SHA256SUMS \
 && ls -1 /app/web/dl

# HTTP и WebSocket — один порт: сервер сам опознаёт запрос на смену
# протокола. Наружу его публикует nginx по https и wss (см. proxy/).
EXPOSE 8080

# --web-root обязателен: вшитый в бинарь путь указывает на исходники,
# которых в рантайм-образе нет. --config-dir смотрит в точку монтирования
# ./mount: там лежит ferry.conf, и это ЕДИНСТВЕННОЕ, что Ferry читает с
# диска. Писать туда сервер не умеет и не будет.
CMD ["/app/FerryServer", "--http-port", "8080", "--ws-port", "9000", \
     "--web-root", "/app/web", "--config-dir", "/app/mount"]
