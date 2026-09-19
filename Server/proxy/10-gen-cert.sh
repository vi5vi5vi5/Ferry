#!/bin/sh
# Bootstrap TLS: гарантирует, что live/ существует ДО старта nginx.
# nginx не стартует, если ssl_certificate указывает на несуществующий файл,
# поэтому шаг обязателен в любом режиме.
#
# Без домена самоподписанный сертификат — финальный. С доменом он временный:
# nginx стартует на нём, а 20-certbot.sh чуть позже переставит live/ на
# Let's Encrypt. Официальный образ nginx сам выполняет скрипты из
# /docker-entrypoint.d/ перед стартом. Всё живёт в томе certs и переживает
# перезапуски — браузерам не приходится принимать исключение заново.
set -e

CERTS=/etc/nginx/certs
SELF_DIR="$CERTS/selfsigned"
LIVE="$CERTS/live"

# Webroot для ACME-challenge: location в nginx.conf на него ссылается всегда.
mkdir -p /var/www/certbot

# Самоподписанный сертификат (домена нет — Let's Encrypt недоступен).
# Генерируется один раз, затем берётся из тома.
if [ ! -f "$SELF_DIR/fullchain.pem" ] || [ ! -f "$SELF_DIR/privkey.pem" ]; then
    echo "Генерация самоподписанного сертификата (10 лет)..."
    mkdir -p "$SELF_DIR"
    openssl req -x509 -nodes -newkey rsa:2048 -days 3650 \
        -subj "/CN=MeetUp" \
        -keyout "$SELF_DIR/privkey.pem" -out "$SELF_DIR/fullchain.pem"
fi

# Если live/ ещё не заведён — направляем на самоподписанный (безопасный старт).
# Если live/ уже указывает на Let's Encrypt после прошлого запуска — не трогаем.
if [ ! -e "$LIVE/fullchain.pem" ]; then
    mkdir -p "$LIVE"
    ln -sf "$SELF_DIR/fullchain.pem" "$LIVE/fullchain.pem"
    ln -sf "$SELF_DIR/privkey.pem"   "$LIVE/privkey.pem"
fi
