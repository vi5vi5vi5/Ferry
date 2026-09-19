# syntax=docker/dockerfile:1.6
# ============================================================
#  Ferry — релей больших файлов.
#
#  Контекст сборки — КОРЕНЬ репозитория (см. docker-compose.yml): образ
#  собирает и сервер, и общее ядро, и CLI-клиент.
#
#  Три стадии сборки, и третья — та самая, ради которой всё:
#
#    build-server  сервер на Qt6. Debian bookworm, пакетный Qt.
#    build-client  клиент, СПЕЦИАЛЬНО на debian bullseye. Причина ниже.
#    runtime       slim-образ: сервер, страницы и бинарь клиента рядом.
#
#  Зачем клиент собирается внутри образа сервера. Затем, что релей раздаёт
#  собственный клиент: `curl -fsSL https://<релей>/install.sh | sh`. Это
#  делает установку такой же простой, как в MeetUp (где клиент — браузер),
#  и гарантирует, что версия клиента совпадает с версией сервера: протокол
#  не может разъехаться по определению.
# ============================================================

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

RUN cmake -S Server -B build-server -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DFERRY_COMMIT="${GIT_COMMIT}" -DFERRY_MODIFIED="${GIT_MODIFIED}" \
 && cmake --build build-server

# ---------- 2. Клиент ----------
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
#
# Понадобится дотянуться дальше назад — это уже не смена базового образа,
# а полностью статическая сборка на musl (alpine). Тогда бинарь пойдёт
# вообще везде, но за это придётся собирать OpenSSL руками.
FROM ubuntu:22.04 AS build-client

# noninteractive: иначе tzdata на ubuntu останавливает сборку вопросом
# про часовой пояс и ждёт ответа до конца времён.
ENV DEBIAN_FRONTEND=noninteractive

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
RUN cmake -S . -B build-client -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DFERRY_STATIC_OPENSSL=ON -DFERRY_COMMIT="${GIT_COMMIT}" \
 && cmake --build build-client \
 && ./build-client/tests/ferry-core-test

# Раскладываем то, что будет раздаваться: бинарь и контрольная сумма.
RUN mkdir -p /out/dl \
 && cp build-client/Cli/ferry "/out/dl/ferry-linux-$(uname -m)" \
 && cd /out/dl && sha256sum ferry-linux-* > SHA256SUMS \
 && cat SHA256SUMS

# ---------- 3. Рантайм ----------
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

# HTTP и WebSocket — один порт: сервер сам опознаёт запрос на смену
# протокола. Наружу его публикует nginx по https и wss (см. proxy/).
EXPOSE 8080

# --web-root обязателен: вшитый в бинарь путь указывает на исходники,
# которых в рантайм-образе нет. --config-dir смотрит в точку монтирования
# ./mount: там лежит ferry.conf, и это ЕДИНСТВЕННОЕ, что Ferry читает с
# диска. Писать туда сервер не умеет и не будет.
CMD ["/app/FerryServer", "--http-port", "8080", "--ws-port", "9000", \
     "--web-root", "/app/web", "--config-dir", "/app/mount"]
