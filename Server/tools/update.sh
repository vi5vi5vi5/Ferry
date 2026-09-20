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
while [[ $# -gt 0 ]]; do
    case "$1" in
        --force|-f)   FORCE=1; shift ;;
        --domain)     export DOMAIN="$2"; shift 2 ;;
        --email)      export LETSENCRYPT_EMAIL="$2"; shift 2 ;;
        --http-port)  export HTTP_PORT="$2"; shift 2 ;;
        --https-port) export HTTPS_PORT="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,18p' "${BASH_SOURCE[0]}" | sed 's/^#//'
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

for var in DOMAIN LETSENCRYPT_EMAIL HTTP_PORT HTTPS_PORT; do
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

if [[ -n "${DOMAIN:-}" ]]; then
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
for spec in "HTTPS:${HTTPS_PORT:-443}" "HTTP:${HTTP_PORT:-80}"; do
    label="${spec%%:*}"
    port="${spec##*:}"
    owner="$(port_owner "$port" || true)"
    [[ -z "$owner" || "$owner" == "__ours__" ]] && continue
    if [[ "$owner" == "__system__" ]]; then
        echo "Порт $port ($label) уже занят каким-то процессом на этом сервере." >&2
    else
        echo "Порт $port ($label) уже занят контейнером ${owner}." >&2
    fi
    CONFLICT=1
done

if [[ "$CONFLICT" -eq 1 ]]; then
    cat >&2 <<'HINT'

Так бывает, когда на сервере уже живёт другой сервис — например MeetUp.
Дайте Ferry свои порты:

  ./tools/update.sh --force --https-port 8443 --http-port 8081

Они запомнятся в .env, дальше флаги указывать не нужно, а релей будет
отвечать по адресу https://<ip-сервера>:8443/

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

echo
echo "=== 2/3 Пересборка и перезапуск (docker compose) ==="
# up -d --build сам пересоберёт изменившиеся образы и перезапустит только
# те контейнеры, которые поменялись. Первая сборка занимает несколько
# минут: собирается сервер на Qt, ядро и клиент.
docker compose up -d --build

echo
echo "=== 3/3 Проверка ==="
docker compose ps
echo

HOST_HINT="<IP-сервера>${HTTPS_PORT:+:$HTTPS_PORT}"
if [[ -n "${DOMAIN:-}" ]]; then
    HOST_HINT="${DOMAIN}"
fi

echo "Готово. HEAD = $NEW_REV (сборка $GIT_COMMIT$( [[ "$GIT_MODIFIED" == "1" ]] && echo ', с локальными изменениями' ))"
echo
echo "Релей:   https://${HOST_HINT}/"
echo "Клиент:  curl -fsSL https://${HOST_HINT}/install.sh | sh"
if [[ -z "${DOMAIN:-}" ]]; then
    echo
    echo "Сертификат самоподписанный, поэтому пока так:"
    echo "  curl -fsSLk https://${HOST_HINT}/install.sh | sh -s -- --insecure"
fi
