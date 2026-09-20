#!/usr/bin/env bash
# ============================================================
#  Ferry за чужим nginx: проверка связки целиком.
#
#  Разыгрывает сервер, где уже живёт другой сервис (у нас — MeetUp) и
#  держит порты 80/443, а Ferry встаёт за его прокси: своего не поднимает,
#  подключается к чужой сети, отзывается по своему server_name.
#
#  Само приложение соседа не нужно — вместо него заглушка. Проверяется
#  ровно то, что может разойтись: nginx-конфиг соседа, include чужих
#  блоков, маршрутизация по имени, сервер по умолчанию, подстановка
#  адреса в установщик и проход вебсокета.
#
#  Запуск:
#    ./tests/shared-proxy.sh                      # MeetUp рядом, в ../MeetUp
#    NEIGHBOUR=/путь/к/MeetUp/Server ./tests/shared-proxy.sh
#
#  Нужен docker. Сертификат самоподписанный — Let's Encrypt к выдуманным
#  именам не придёт, а всё остальное от этого не меняется.
# ============================================================
set -u

export MSYS_NO_PATHCONV=1   # Git Bash на Windows иначе портит пути

FERRY_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../Server" && pwd)"
NEIGHBOUR="${NEIGHBOUR:-$(cd "$FERRY_DIR/../.." && pwd)/MeetUp/Server}"
NET="${NET:-ferry-shared-proxy-test}"

if [[ ! -f "$NEIGHBOUR/proxy/nginx.conf" ]]; then
    echo "Не нашёл соседа: $NEIGHBOUR/proxy/nginx.conf" >&2
    echo "Укажите путь: NEIGHBOUR=/путь/к/MeetUp/Server $0" >&2
    exit 1
fi

# Пути для docker. Git Bash на Windows отдаёт /c/Users/..., а docker ждёт
# C:/Users/... — без этого не соберётся контекст и не смонтируется том.
# На Linux cygpath нет, и путь уходит как есть.
to_host_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -m "$1"
    else
        echo "$1"
    fi
}
NEIGHBOUR_HOST="$(to_host_path "$NEIGHBOUR")"
FERRY_HOST="$(to_host_path "$FERRY_DIR")"

PASS=0; FAIL=0
ok()  { echo "  [ok]   $1"; PASS=$((PASS+1)); }
bad() { echo "  [ПРОВАЛ] $1"; FAIL=$((FAIL+1)); }

cleanup() {
    docker rm -f ferry-test-neighbour-proxy ferry-test-neighbour-app >/dev/null 2>&1
    (cd "$FERRY_DIR" && FERRY_PROXY_NETWORK=$NET docker compose \
        -f docker-compose.yml -f docker-compose.behind-proxy.yml down >/dev/null 2>&1)
    docker network rm "$NET" >/dev/null 2>&1
    rm -f "$NEIGHBOUR/proxy/extra/ferry-test.conf"
}
trap cleanup EXIT
cleanup

echo "=== 1. Сеть соседа и его прокси ==="
docker network create "$NET" >/dev/null
docker run -d --name ferry-test-neighbour-app --network "$NET" --network-alias app \
    nginx:alpine >/dev/null
docker build -q -t ferry-test-neighbour-proxy "$NEIGHBOUR_HOST/proxy" >/dev/null \
    || { echo "прокси соседа не собрался" >&2; exit 1; }
mkdir -p "$NEIGHBOUR/proxy/extra"
docker run -d --name ferry-test-neighbour-proxy --network "$NET" \
    -v "$NEIGHBOUR_HOST/proxy/extra:/etc/nginx/extra:ro" \
    ferry-test-neighbour-proxy >/dev/null
sleep 4
if docker exec ferry-test-neighbour-proxy nginx -t 2>&1 | grep -q successful; then
    ok "прокси соседа поднялся с пустым каталогом extra/"
else
    bad "прокси не стартовал на пустом extra/"
    docker logs ferry-test-neighbour-proxy 2>&1 | tail -5
fi
PROXY_IP=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' \
    ferry-test-neighbour-proxy)

echo
echo "=== 2. Ferry встаёт за него, своего прокси не поднимая ==="
cd "$FERRY_DIR"
FERRY_PROXY_NETWORK=$NET docker compose \
    -f docker-compose.yml -f docker-compose.behind-proxy.yml up -d --build 2>&1 | tail -2
sleep 4
if docker ps --format '{{.Names}}' | grep -q '^ferry-proxy'; then
    bad "прокси Ferry всё-таки запустился — он бы занял порты соседа"
else
    ok "прокси Ferry не запускался"
fi

echo
echo "=== 3. Ferry виден из сети соседа как ferry-app ==="
if docker run --rm --network "$NET" curlimages/curl:latest -s --max-time 5 \
        http://ferry-app:8080/api/health 2>/dev/null | grep -q '"product":"ferry"'; then
    ok "ferry-app:8080 отвечает"
else
    bad "ferry-app:8080 недоступен — алиас в сети не тот"
fi

echo
echo "=== 4. Соседу кладётся server-блок Ferry ==="
sed 's/server_name fferry\.ru;/server_name ferry.test;/' \
    "$FERRY_DIR/proxy/ferry.conf.example" > "$NEIGHBOUR/proxy/extra/ferry-test.conf"
docker exec ferry-test-neighbour-proxy nginx -s reload >/dev/null 2>&1
sleep 2
if docker exec ferry-test-neighbour-proxy nginx -t 2>&1 | grep -q successful; then
    ok "конфиг Ferry принят nginx соседа"
else
    bad "nginx не принял конфиг Ferry"
    docker exec ferry-test-neighbour-proxy nginx -t 2>&1 | tail -6
fi

ask() {   # $1 = имя, $2 = путь
    docker run --rm --network "$NET" curlimages/curl:latest -sk --max-time 5 \
        --resolve "$1:443:$PROXY_IP" "https://$1$2" 2>/dev/null
}

echo
echo "=== 5. Маршрутизация по имени ==="
FERRY_BODY="$(ask ferry.test /api/health)"
MEET_BODY="$(ask neighbour.test /)"
DEFAULT_BODY="$(docker run --rm --network "$NET" curlimages/curl:latest -sk --max-time 5 \
    "https://$PROXY_IP/" 2>/dev/null)"

case "$FERRY_BODY" in
    *'"product":"ferry"'*) ok "ferry.test попадает в Ferry" ;;
    *) bad "ferry.test ушёл не туда: $(echo "$FERRY_BODY" | head -c 60)" ;;
esac

# Заглушка соседа — обычный nginx с его приветственной страницей. Ferry на
# этом месте отдал бы свою, с lang="ru": различие однозначное.
#
# Проверка не формальная. Пока сервер по умолчанию не был помечен явно,
# сюда попадал именно Ferry: первый описанный server-блок для listen 443
# становится default, а порядок зависел от того, когда сработал include
# чужих конфигов. Сосед молча отдавал весь неопознанный трафик соседу.
case "$MEET_BODY" in
    *"Welcome to nginx"*) ok "имя соседа осталось у соседа" ;;
    *) bad "запрос к соседу ушёл не к нему" ;;
esac
case "$DEFAULT_BODY" in
    *"Welcome to nginx"*) ok "безымянный запрос достаётся соседу, а не Ferry" ;;
    *) bad "сервером по умолчанию стал не сосед" ;;
esac

echo
echo "=== 6. Установщик подставляет имя Ferry, а не соседа ==="
INST="$(ask ferry.test /install.sh | grep -E '^RELAY_')"
echo "$INST" | sed 's/^/  /'
case "$INST" in
    *'RELAY_HOST="ferry.test"'*) ok "RELAY_HOST — имя Ferry" ;;
    *) bad "не то имя в install.sh" ;;
esac
case "$INST" in
    *'RELAY_SCHEME="https"'*) ok "RELAY_SCHEME — https" ;;
    *) bad "схема не https" ;;
esac

echo
echo "=== 7. Вебсокет проходит через общий nginx ==="
UP="$(docker run --rm --network "$NET" curlimages/curl:latest -sk --max-time 5 -i \
     --resolve "ferry.test:443:$PROXY_IP" \
     -H "Connection: Upgrade" -H "Upgrade: websocket" \
     -H "Sec-WebSocket-Version: 13" -H "Sec-WebSocket-Key: AAAAAAAAAAAAAAAAAAAAAA==" \
     "https://ferry.test/wsf" 2>/dev/null | head -1)"
case "$UP" in
    *101*) ok "смена протокола проходит (101)" ;;
    *) bad "апгрейд не прошёл: $UP" ;;
esac

echo
echo "=== 8. Сосед переживает выключенный Ferry ==="
# Самая неприятная связность, которую тут можно случайно завести: nginx
# разрешает имена из proxy_pass при загрузке конфига, и выключенный Ferry
# не дал бы соседу стартовать вообще. Проверяем, что не так.
(cd "$FERRY_DIR" && FERRY_PROXY_NETWORK=$NET docker compose     -f docker-compose.yml -f docker-compose.behind-proxy.yml down >/dev/null 2>&1)
sleep 2
docker restart ferry-test-neighbour-proxy >/dev/null 2>&1
sleep 4
if docker ps --format '{{.Names}}' | grep -q '^ferry-test-neighbour-proxy$'; then
    ok "сосед поднялся при выключенном Ferry"
else
    bad "сосед не стартовал без Ferry"
    docker logs ferry-test-neighbour-proxy 2>&1 | tail -4
fi
STILL="$(docker run --rm --network "$NET" curlimages/curl:latest -sk --max-time 5     "https://$PROXY_IP/" 2>/dev/null)"
case "$STILL" in
    *"Welcome to nginx"*) ok "сосед обслуживает свой домен как ни в чём не бывало" ;;
    *) bad "сосед перестал отвечать" ;;
esac
CODE="$(docker run --rm --network "$NET" curlimages/curl:latest -sk --max-time 5 -o /dev/null     -w '%{http_code}' --resolve "ferry.test:443:$PROXY_IP" https://ferry.test/api/health 2>/dev/null)"
case "$CODE" in
    50*) ok "домен Ferry честно отвечает $CODE, а не роняет nginx" ;;
    *) bad "неожиданный код при выключенном Ferry: $CODE" ;;
esac

echo
echo "================================"
echo "прошло: $PASS, провалено: $FAIL"
echo "================================"
[[ $FAIL -eq 0 ]]
