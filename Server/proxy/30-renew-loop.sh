#!/bin/sh
# Автопродление Let's Encrypt. Активно только если задан DOMAIN.
# Уходит в фон сразу — иначе синхронный entrypoint не даст стартовать nginx.
#
# Раз в 12 часов пробует renew через тот же webroot (nginx уже держит :80
# и отдаёт /.well-known/acme-challenge/). certbot renew ничего не запрашивает,
# пока до истечения больше 30 дней, — цикл безопасен для rate limit.
# --deploy-hook срабатывает ТОЛЬКО при реальном обновлении: там reload nginx.
set -e

[ -z "$DOMAIN" ] && exit 0

WEBROOT=/var/www/certbot

(
    # Даём nginx стартовать и, если нужно, 20-certbot.sh получить первый сертификат.
    sleep 300
    while true; do
        certbot renew \
            --webroot -w "$WEBROOT" \
            --deploy-hook "nginx -s reload" \
            >> /var/log/certbot-renew.log 2>&1 || true
        sleep 12h
    done
) &
