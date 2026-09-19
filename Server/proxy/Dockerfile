# nginx-терминатор TLS для Ferry: снаружи https/wss, внутри обычные http/ws.
FROM nginx:alpine

# openssl — самоподписанный сертификат (fallback без домена);
# certbot — Let's Encrypt по HTTP-01, когда задан домен.
RUN apk add --no-cache openssl certbot

COPY nginx.conf /etc/nginx/conf.d/default.conf

# Скрипты выполняются entrypoint-ом nginx по возрастанию номера:
#   10 — bootstrap самоподписанного + live/ (всегда);
#   20 — получение Let's Encrypt (если задан DOMAIN);
#   30 — фоновое автопродление (если задан DOMAIN).
COPY 10-gen-cert.sh   /docker-entrypoint.d/10-gen-cert.sh
COPY 20-certbot.sh    /docker-entrypoint.d/20-certbot.sh
COPY 30-renew-loop.sh /docker-entrypoint.d/30-renew-loop.sh
RUN chmod +x /docker-entrypoint.d/*.sh
