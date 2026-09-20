#!/bin/sh
# Let's Encrypt через HTTP-01 (webroot). Работает только если задан DOMAIN;
# без домена сразу выходит и остаётся самоподписанный сертификат.
#
# Официальный entrypoint выполняет скрипты из /docker-entrypoint.d/
# синхронно и ДО старта nginx, поэтому запрос делаем в фоне: дожидаемся,
# пока nginx займёт :80 и начнёт отдавать /.well-known/acme-challenge/,
# затем запрашиваем сертификат и переставляем live/ на Let's Encrypt.
set -e

[ -z "$DOMAIN" ] && exit 0

CERTS=/etc/nginx/certs
LIVE="$CERTS/live"
LE_LIVE="/etc/letsencrypt/live/$DOMAIN"
WEBROOT=/var/www/certbot

# Уже есть сертификат для этого домена — не запрашиваем повторно
# (у Let's Encrypt жёсткие rate limit). live/ переставит блок ниже.
if [ ! -d "$LE_LIVE" ]; then
    (
        # Ждём, пока nginx поднимется и начнёт слушать :80 — иначе HTTP-01
        # challenge не пройдёт. Пробуем до ~30 секунд.
        i=0
        while [ "$i" -lt 30 ]; do
            if wget -q -O /dev/null "http://127.0.0.1/.well-known/acme-challenge/" 2>/dev/null \
               || nc -z 127.0.0.1 80 2>/dev/null; then
                break
            fi
            i=$((i + 1))
            sleep 1
        done

        EMAIL_ARG="--register-unsafely-without-email"
        [ -n "$LETSENCRYPT_EMAIL" ] && EMAIL_ARG="--email $LETSENCRYPT_EMAIL"

        echo "Запрос сертификата Let's Encrypt для $DOMAIN (HTTP-01, webroot)..."
        # || остаёмся на самоподписанном: контейнер работает в любом случае.
        if certbot certonly --webroot -w "$WEBROOT" \
            --non-interactive --agree-tos $EMAIL_ARG \
            -d "$DOMAIN" \
            --keep-until-expiring; then

            ln -sf "$LE_LIVE/fullchain.pem" "$LIVE/fullchain.pem"
            ln -sf "$LE_LIVE/privkey.pem"   "$LIVE/privkey.pem"
            nginx -s reload
            echo "live/ → Let's Encrypt ($DOMAIN); nginx перечитал сертификат."
        else
            echo "Не удалось получить LE-сертификат — остаёмся на самоподписанном."
        fi
    ) &
else
    # Сертификат уже есть (прошлый запуск) — убеждаемся, что live/ на него смотрит.
    ln -sf "$LE_LIVE/fullchain.pem" "$LIVE/fullchain.pem"
    ln -sf "$LE_LIVE/privkey.pem"   "$LIVE/privkey.pem"
    echo "Сертификат Let's Encrypt для $DOMAIN уже есть — live/ на него."
fi
