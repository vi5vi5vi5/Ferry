#!/usr/bin/env bash
# ============================================================
#  Ferry — обновление с GitHub и пересборка (docker compose)
#
#  Использование:
#    ./tools/update.sh                  обновить (пересобрать, если есть изменения)
#    ./tools/update.sh --force          пересобрать и перезапустить в любом случае
#
#  Выбор режима TLS:
#    ./tools/update.sh                                        самоподписанный (по умолчанию)
#    ./tools/update.sh --domain ferry.example.ru --email you@mail.com   Let's Encrypt
#
#  Переопределить порты хоста (с доменом HTTP_PORT переопределять нельзя):
#    ./tools/update.sh --https-port 8443 --http-port 8081
#
#  Если 80 и 443 уже держит сосед (например MeetUp) — встать за его nginx:
#    ./tools/update.sh --behind-proxy server_default
#  Свой прокси тогда не поднимается, Ferry подключается к сети соседа, а
#  тому кладётся готовый server-блок из proxy/ferry.conf.example.
#
#  Не нужен клиент под Windows — не собирать его (экономит несколько минут
#  ПЕРВОЙ сборки; дальше стадия всё равно берётся из кэша):
#    ./tools/update.sh --skip-windows
#
#  Маленькая машина (два потока, два гигабайта)? Стадии и так идут по
#  очереди, но внутри стадии ninja на двух ядрах запускает три
#  компиляции разом. Если сборка падает по памяти — прижать:
#    ./tools/update.sh --jobs 1
#
#  Домен, почта и порты ЗАПОМИНАЮТСЯ в .env рядом с docker-compose.yml:
#  указали --domain один раз — дальше хватает `./tools/update.sh`.
# ============================================================
set -euo pipefail

# Скрипт лежит в Server/tools/; compose — в Server/; репозиторий — выше.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$SERVER_DIR/.." && pwd)"
cd "$SERVER_DIR"

FORCE=0
JOBS_RESET=0
STANDALONE_RESET=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --force|-f)   FORCE=1; shift ;;
        --domain)     export DOMAIN="$2"; shift 2 ;;
        --email)      export LETSENCRYPT_EMAIL="$2"; shift 2 ;;
        --http-port)  export HTTP_PORT="$2"; shift 2 ;;
        --https-port) export HTTPS_PORT="$2"; shift 2 ;;
        --behind-proxy)
            if [[ -z "${2:-}" || "${2:0:1}" == "-" ]]; then
                echo "У --behind-proxy нужно имя docker-сети соседа." >&2
                echo "Посмотреть: docker network ls" >&2
                exit 1
            fi
            export FERRY_PROXY_NETWORK="$2"; shift 2 ;;
        --standalone) STANDALONE_RESET=1; export FERRY_PROXY_NETWORK=""; shift ;;
        --jobs|-j)
            case "${2:-}" in
                ''|*[!0-9]*)
                    echo "У --jobs нужно число: сколько компиляций разом." >&2
                    exit 1 ;;
            esac
            export BUILD_JOBS="$2"; shift 2 ;;
        --jobs-auto)     JOBS_RESET=1; shift ;;
        --skip-windows)  export WINDOWS_STAGE="build-client-windows-skip"; shift ;;
        --with-windows)  export WINDOWS_STAGE="build-client-windows"; shift ;;
        -h|--help)
            sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^#//'
            exit 0 ;;
        *)
            echo "Неизвестный аргумент: $1" >&2
            echo "Запустите ./tools/update.sh --help" >&2
            exit 1 ;;
    esac
done

# ---- .env: настройки установки, которые незачем вводить каждый раз ----
# docker compose читает этот файл сам, поэтому запомненного домена хватает
# и для голого `docker compose up -d`. Окружение процесса сильнее файла —
# то есть флаг текущего запуска всегда побеждает запомненное.
ENV_FILE="$SERVER_DIR/.env"

env_file_get() {
    [[ -f "$ENV_FILE" ]] || return 0
    sed -n "s/^$1=//p" "$ENV_FILE" | tail -n 1
}

# Переписываем файл целиком, а не правим строку на месте: в домене и почте
# попадаются символы, которые пришлось бы экранировать для sed.
env_file_set() {
    local key="$1" value="$2"
    touch "$ENV_FILE"
    local tmp="${ENV_FILE}.tmp"
    grep -v "^${key}=" "$ENV_FILE" > "$tmp" || true
    printf '%s=%s\n' "$key" "$value" >> "$tmp"
    mv "$tmp" "$ENV_FILE"
}

env_file_unset() {
    [[ -f "$ENV_FILE" ]] || return 0
    local tmp="${ENV_FILE}.tmp"
    grep -v "^$1=" "$ENV_FILE" > "$tmp" || true
    mv "$tmp" "$ENV_FILE"
}

# «Верни как было» надо отработать ДО цикла ниже. Иначе пустое значение
# проскочит мимо ветки «запомнить» и тут же будет перезаполнено старым
# из .env — то есть флаг не сделает ровно ничего.
if [[ "$JOBS_RESET" == "1" ]]; then
    env_file_unset BUILD_JOBS
    unset BUILD_JOBS
    echo "Число компиляций разом больше не ограничено."
fi

# То же самое с --standalone, и здесь это стоило дороже: однажды
# запомненная сеть соседа возвращалась при каждом запуске, и уйти из-за
# чужого nginx обратно на свой прокси было нельзя никаким флагом.
if [[ "$STANDALONE_RESET" == "1" && -z "${FERRY_PROXY_NETWORK:-}" ]]; then
    env_file_unset FERRY_PROXY_NETWORK
    unset FERRY_PROXY_NETWORK
fi

for var in DOMAIN LETSENCRYPT_EMAIL HTTP_PORT HTTPS_PORT FERRY_PROXY_NETWORK WINDOWS_STAGE BUILD_JOBS; do
    if [[ -n "${!var:-}" ]]; then
        env_file_set "$var" "${!var}"
    else
        remembered="$(env_file_get "$var")"
        if [[ -n "$remembered" ]]; then
            export "$var=$remembered"
        fi
    fi
done

# ---- mount/: единственное, что Ferry читает с диска ----
# Каталог создаём заранее и кладём в него закомментированный конфиг. Иначе
# docker создал бы его от root, и владелец сервера не смог бы туда писать
# без sudo — мелочь, на которую тратится полчаса в самый неподходящий момент.
if [[ ! -d "$SERVER_DIR/mount" ]]; then
    mkdir -p "$SERVER_DIR/mount"
fi
if [[ ! -f "$SERVER_DIR/mount/ferry.conf" ]] && [[ -f "$SERVER_DIR/mount/ferry.conf.example" ]]; then
    cp "$SERVER_DIR/mount/ferry.conf.example" "$SERVER_DIR/mount/ferry.conf"
    echo "Создан mount/ferry.conf — все настройки закомментированы, работаем на умолчаниях."
fi

# ---- Режим: сам себе прокси или за чужим ----
#
# Вычисляется до первого обращения к compose: от него зависит и набор
# файлов, и профиль, и нужна ли вообще проверка портов.
if [[ -n "${FERRY_PROXY_NETWORK:-}" ]]; then
    BEHIND_PROXY=1
    COMPOSE_ARGS=(-f docker-compose.yml -f docker-compose.behind-proxy.yml)
    # Записываем в .env, чтобы голый `docker compose up -d` в этом
    # каталоге вёл себя так же, как update.sh, а не поднимал вдруг
    # собственный прокси на занятые порты.
    env_file_set COMPOSE_FILE "docker-compose.yml:docker-compose.behind-proxy.yml"
    env_file_set COMPOSE_PROFILES ""
    export COMPOSE_PROFILES=""
    # За чужим nginx своих портов наружу нет — и проверять нечего.
    PORTS_TO_CHECK=()
else
    BEHIND_PROXY=0
    COMPOSE_ARGS=(-f docker-compose.yml)
    env_file_set COMPOSE_FILE "docker-compose.yml"
    env_file_set COMPOSE_PROFILES "standalone"
    export COMPOSE_PROFILES="standalone"
    # Ровно те порты, что публикует proxy в docker-compose.yml.
    #
    # Задать этот список когда-то забыли, и без него проверка ниже молча
    # крутилась по пустому массиву: ${X[@]+...} написан ровно так, чтобы
    # set -u не ругался на незаданный массив. Вместо подсказки человек
    # получал сырое «Bind for 0.0.0.0:80 failed» из недр docker.
    PORTS_TO_CHECK=("http:${HTTP_PORT:-80}" "https:${HTTPS_PORT:-443}")
fi

if [[ "$BEHIND_PROXY" -eq 1 ]]; then
    echo "Режим: за чужим nginx, сеть ${FERRY_PROXY_NETWORK}."
    echo "  Свой прокси не поднимается; TLS и порты — забота соседа."
    if ! docker network inspect "$FERRY_PROXY_NETWORK" >/dev/null 2>&1; then
        echo >&2
        echo "Но сети ${FERRY_PROXY_NETWORK} на этой машине нет." >&2
        echo "Посмотреть, какие есть:  docker network ls" >&2
        echo "У MeetUp это обычно server_default (имя проекта + _default)." >&2
        exit 1
    fi
    if [[ -n "${DOMAIN:-}" ]]; then
        echo "  (--domain в этом режиме не используется: сертификат выписывает сосед)"
    fi
elif [[ -n "${DOMAIN:-}" ]]; then
    echo "Режим TLS: Let's Encrypt для домена ${DOMAIN}"
    if [[ -z "${LETSENCRYPT_EMAIL:-}" ]]; then
        echo "  (email не задан — сертификат выпустится, но без уведомлений об истечении;"
        echo "   рекомендуется --email you@mail.com)"
    fi
else
    echo "Режим TLS: самоподписанный сертификат (домен не задан)."
    echo "  Клиенту понадобится --insecure, а браузер предупредит о безопасности."
    echo "  Есть домен? ./tools/update.sh --force --domain ваш-домен --email вы@почта"
fi

# ---- Осадок от старого имени проекта ----
#
# До появления `name: ferry` в docker-compose.yml compose брал имя проекта
# от каталога — «server», ровно как у MeetUp. На сервере, где стоят оба,
# запуск Ferry подменял контейнеры MeetUp своими: совпадали и имя проекта,
# и имена сервисов. Теперь это невозможно, но контейнеры, поднятые ДО
# исправления, могут до сих пор работать под чужим именем и держать порты.
#
# Ищем ровно их: образ наш, имя проекта — не наше.
STALE="$(docker ps -a \
    --format '{{.Names}}|{{.Image}}|{{.Label "com.docker.compose.project"}}' 2>/dev/null \
    | awk -F'|' '$2 ~ /ferry/ && $3 != "" && $3 != "ferry" { print "  " $1 "  (проект " $3 ", образ " $2 ")" }' \
    || true)"

if [[ -n "$STALE" ]]; then
    cat >&2 <<HINT
На этой машине остались контейнеры Ferry, поднятые под ЧУЖИМ именем проекта:

$STALE

Так выглядит след старой ошибки: до исправления Ferry занимал имя проекта
«server» и подменял собой контейнеры MeetUp. Если MeetUp перестал отвечать —
это оно.

Порядок восстановления:

  1. Вернуть соседа на место (он заберёт своё имя проекта обратно):
       cd ../../MeetUp/Server && ./tools/update.sh --force
     Тома с сертификатами Let's Encrypt привязаны к имени проекта и никуда
     не делись, так что сертификат переживёт это без потерь.

  2. Вернуться сюда и запустить снова — теперь Ferry живёт под своим
     именем и чужого не трогает.

HINT
    exit 1
fi

# ---- Порты: заняты ли они кем-то посторонним ----
#
# Самая частая беда при втором сервисе на одном сервере: на 80 и 443 уже
# сидит чужой nginx (например от MeetUp), docker compose падает с
# «address already in use», а человек остаётся с наполовину поднятым
# стеком и невнятной строкой в выводе. Сказать об этом ДО сборки стоит
# десяти строк.
#
# Наши же контейнеры от прошлого запуска — не помеха: compose их
# пересоздаст. Поэтому их из проверки исключаем.
port_owner() {
    local port="$1" id name
    for id in $(docker ps -q 2>/dev/null); do
        if docker port "$id" 2>/dev/null | grep -qE ":${port}\$"; then
            case " ${OUR_CONTAINERS} " in
                *" ${id} "*) echo "__ours__"; return 0 ;;
            esac
            name="$(docker inspect -f '{{.Name}}' "$id" 2>/dev/null | sed 's|^/||')"
            echo "${name:-другой контейнер}"
            return 0
        fi
    done
    if command -v ss >/dev/null 2>&1; then
        if ss -Hltn 2>/dev/null | grep -qE "[:.]${port}[[:space:]]"; then
            echo "__system__"
            return 0
        fi
    fi
    return 1
}

OUR_CONTAINERS="$(docker compose ps -q 2>/dev/null | tr '\n' ' ' || true)"
CONFLICT=0
CONFLICT_OWNER=""
for spec in ${PORTS_TO_CHECK[@]+"${PORTS_TO_CHECK[@]}"}; do
    label="${spec%%:*}"
    port="${spec##*:}"
    owner="$(port_owner "$port" || true)"
    [[ -z "$owner" || "$owner" == "__ours__" ]] && continue
    if [[ "$owner" == "__system__" ]]; then
        echo "Порт $port ($label) уже занят каким-то процессом на этом сервере." >&2
    else
        echo "Порт $port ($label) уже занят контейнером ${owner}." >&2
        [[ -z "$CONFLICT_OWNER" ]] && CONFLICT_OWNER="$owner"
    fi
    CONFLICT=1
done

if [[ "$CONFLICT" -eq 1 ]]; then
    # В какой сети живёт тот, кто занял порт. Именно её надо передать в
    # --behind-proxy, и угадывать её — лишний раз ошибиться: имя составляется
    # из имени проекта compose и на разных серверах бывает разным.
    NEIGHBOR_NET=""
    if [[ -n "$CONFLICT_OWNER" ]]; then
        NEIGHBOR_NET="$(docker inspect -f '{{range $k, $v := .NetworkSettings.Networks}}{{$k}} {{end}}' "$CONFLICT_OWNER" 2>/dev/null \
            | tr ' ' '\n' | grep -vE '^(bridge|host|none)?$' | head -n 1 || true)"
    fi
    NEIGHBOR_HINT="${NEIGHBOR_NET:-server_default}"
    cat >&2 <<HINT

Так бывает, когда на сервере уже живёт другой сервис — например MeetUp.
Есть два выхода.

1. Встать ЗА его nginx — тогда у Ferry будет свой домен без порта в
   ссылке и настоящий сертификат:

     ./tools/update.sh --force --behind-proxy ${NEIGHBOR_HINT}

   Соседу при этом кладётся готовый server-блок; как именно — напишем
   после запуска.

2. Взять свои порты и жить отдельно, на самоподписанном сертификате:

     ./tools/update.sh --force --https-port 8443 --http-port 8081

Выбранное запомнится в .env, дальше флаги указывать не нужно.

HINT
    exit 1
fi

echo
echo "=== 1/3 Получение новой версии из GitHub ==="
OLD_REV="$(git rev-parse HEAD 2>/dev/null || echo none)"

git pull --ff-only

NEW_REV="$(git rev-parse HEAD 2>/dev/null || echo none)"

if [[ "$FORCE" -eq 0 && "$OLD_REV" == "$NEW_REV" ]]; then
    echo "Новых коммитов нет (HEAD = $NEW_REV)."
    echo "Пересборка не требуется. Запустите с --force, чтобы пересобрать принудительно."
    exit 0
fi

# Из какого коммита собираем: сервер отдаёт это в GET /api/health, а
# клиент — в `ferry version`. Внутри образа гита нет (см. .dockerignore),
# поэтому считаем здесь и передаём аргументами сборки.
#
# ПОСЛЕ git pull, а не до: считали бы до — и в бинарь попадал бы номер
# коммита, из которого мы уходим.
#
# Смотрим на весь репозиторий, кроме docs: в образ едет и сервер, и ядро,
# и клиент. Untracked-файлы не считаем — заметка, забытая рядом с
# исходниками, в бинарь не попадает.
GIT_COMMIT="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
if [[ -n "$(git -C "$REPO_ROOT" status --porcelain -uno -- core Cli Server 2>/dev/null)" ]]; then
    GIT_MODIFIED=1
else
    GIT_MODIFIED=0
fi
export GIT_COMMIT GIT_MODIFIED

if [[ "${WINDOWS_STAGE:-}" == "build-client-windows-skip" ]]; then
    echo "Клиент под Windows не собирается (--skip-windows)."
    echo "  Вернуть: ./tools/update.sh --force --with-windows"
fi

if [[ -n "${BUILD_JOBS:-}" ]]; then
    echo "Компиляций разом: ${BUILD_JOBS} (запомнено в .env)."
    echo "  Снять ограничение: ./tools/update.sh --force --jobs-auto"
fi

echo
echo "=== 2/3 Пересборка и перезапуск (docker compose) ==="
echo "Первая сборка занимает несколько минут: Qt-сервер, ядро и два клиента."
echo "Стадии идут по очереди, а не разом: три компилятора параллельно не"
echo "выживают на маленькой машине. Из-за этого сборка длиннее, но доходит до конца."
echo "Повторные — быстрые, тяжёлые стадии берутся из кэша docker."
# up -d --build сам пересоберёт изменившиеся образы и перезапустит только
# те контейнеры, которые поменялись. Первая сборка занимает несколько
# минут: собирается сервер на Qt, ядро и клиент.
docker compose "${COMPOSE_ARGS[@]}" up -d --build

# За чужим nginx собственный прокси не нужен, но сам он никуда не денется:
# сервис proxy в compose-файле есть, просто под профилем standalone, и
# сиротой для --remove-orphans не считается. Так на сервере остаётся
# висеть контейнер от прошлого запуска — либо живой и держащий 80 и 443
# (если раньше жили сами по себе), либо мёртвый, так и не поднявшийся на
# занятых портах. Убираем только его и только в этом режиме.
if [[ "$BEHIND_PROXY" -eq 1 ]]; then
    docker compose -f docker-compose.yml --profile standalone rm -sf proxy >/dev/null 2>&1 || true
fi

echo
echo "=== 3/3 Проверка ==="
docker compose "${COMPOSE_ARGS[@]}" ps
echo

echo "Готово. HEAD = $NEW_REV (сборка $GIT_COMMIT$( [[ "$GIT_MODIFIED" == "1" ]] && echo ', с локальными изменениями' ))"
echo

if [[ "$BEHIND_PROXY" -eq 1 ]]; then
    cat <<HINT
Ferry поднят и ждёт запросов от соседского nginx под именем ferry-app:8080.
Снаружи он пока не виден — осталось сказать соседу, куда ходить.

Один раз на стороне того сервиса, который держит 80 и 443 (у MeetUp это
MeetUp/Server):

  1. Положить server-блок и вписать в него своё имя домена:

       mkdir -p proxy/extra
       cp $SERVER_DIR/proxy/ferry.conf.example proxy/extra/ferry.conf
       \$EDITOR proxy/extra/ferry.conf        # заменить server_name

  2. Добавить это имя в сертификат и перезапустить прокси:

       ./tools/update.sh --force --extra-domain <ваш-домен>

Проверить, что Ferry виден изнутри сети соседа:

  docker run --rm --network ${FERRY_PROXY_NETWORK} curlimages/curl -s http://ferry-app:8080/api/health

HINT
else
    HOST_HINT="<IP-сервера>${HTTPS_PORT:+:$HTTPS_PORT}"
    if [[ -n "${DOMAIN:-}" ]]; then
        HOST_HINT="${DOMAIN}"
    fi
    echo "Релей:   https://${HOST_HINT}/"
    echo "Клиент:  curl -fsSL https://${HOST_HINT}/install.sh | sh"
    if [[ -z "${DOMAIN:-}" ]]; then
        echo
        echo "Сертификат самоподписанный, поэтому пока так:"
        echo "  curl -fsSLk https://${HOST_HINT}/install.sh | sh -s -- --insecure"
    fi
fi
