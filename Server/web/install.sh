#!/bin/sh
# ============================================================
#  Ferry — установка клиента.
#
#      curl -fsSL https://<релей>/install.sh | sh
#
#  Скрипт отдаёт сам релей, и адрес релея он подставляет сюда на лету —
#  поэтому после установки `ferry send файл` работает без единого флага.
#
#  Что происходит: скачивается один файл, сверяется его sha256, файл
#  кладётся в PATH, адрес релея пишется в ~/.config/ferry/config. Ничего
#  больше: ни пакетов, ни служб, ни правки системных настроек.
#
#  Удалить: rm <куда положили>/ferry && rm -rf ~/.config/ferry
# ============================================================
set -eu

RELAY_HOST="@FERRY_RELAY@"
RELAY_SCHEME="@FERRY_SCHEME@"
BASE="${RELAY_SCHEME}://${RELAY_HOST}"

INSECURE=0
PREFIX=""

while [ $# -gt 0 ]; do
    case "$1" in
        --insecure|-k) INSECURE=1; shift ;;
        --prefix)      PREFIX="$2"; shift 2 ;;
        --help|-h)
            echo "Использование: curl -fsSL ${BASE}/install.sh | sh [-s -- ключи]"
            echo "  --insecure     не проверять сертификат релея (самоподписанный)"
            echo "  --prefix <dir> куда положить бинарь"
            exit 0 ;;
        *) echo "Неизвестный ключ: $1" >&2; exit 2 ;;
    esac
done

CURL="curl -fsSL"
if [ "$INSECURE" = "1" ]; then
    CURL="curl -fsSLk"
    echo "ВНИМАНИЕ: сертификат релея не проверяется."
    echo "Так бывает, пока у релея нет домена. Посредник в сети может выдать"
    echo "себя за него. Когда домен появится, флаг станет не нужен."
    echo
fi

# ---- 1. Архитектура ----
ARCH="$(uname -m)"
OS="$(uname -s)"
if [ "$OS" != "Linux" ]; then
    echo "Пока есть только сборка под Linux (у вас: $OS)." >&2
    echo "Соберите из исходников: https://github.com/vi5vi5vi5/Ferry" >&2
    exit 1
fi
case "$ARCH" in
    x86_64|amd64)  ASSET="ferry-linux-x86_64" ;;
    aarch64|arm64) ASSET="ferry-linux-aarch64" ;;
    *)
        echo "Для архитектуры $ARCH готового бинаря нет." >&2
        echo "Соберите из исходников — это полминуты:" >&2
        echo "  sudo apt install -y build-essential cmake libssl-dev git" >&2
        echo "  git clone https://github.com/vi5vi5vi5/Ferry.git" >&2
        echo "  cd Ferry && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build" >&2
        exit 1 ;;
esac

# ---- 2. Куда класть ----
if [ -n "$PREFIX" ]; then
    DEST="$PREFIX"
elif [ -w /usr/local/bin ] 2>/dev/null; then
    DEST=/usr/local/bin
else
    DEST="$HOME/.local/bin"
fi
mkdir -p "$DEST"

# ---- 3. Скачиваем и сверяем ----
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "Скачиваю ${ASSET} с ${RELAY_HOST}…"
$CURL "${BASE}/dl/${ASSET}" -o "$TMP/ferry"
$CURL "${BASE}/dl/SHA256SUMS" -o "$TMP/SHA256SUMS" || true

# Сверка обязательна, если список сумм получен: скачанный и запускаемый
# бинарь — это ровно то место, где молчаливое «ну и ладно» неуместно.
if [ -s "$TMP/SHA256SUMS" ]; then
    WANT="$(grep " ${ASSET}\$" "$TMP/SHA256SUMS" | awk '{print $1}')"
    if [ -n "$WANT" ]; then
        if command -v sha256sum >/dev/null 2>&1; then
            GOT="$(sha256sum "$TMP/ferry" | awk '{print $1}')"
        elif command -v shasum >/dev/null 2>&1; then
            GOT="$(shasum -a 256 "$TMP/ferry" | awk '{print $1}')"
        else
            GOT=""
        fi
        if [ -n "$GOT" ] && [ "$GOT" != "$WANT" ]; then
            echo "Контрольная сумма не сошлась — файл по дороге подменили или побился." >&2
            echo "  ожидалось: $WANT" >&2
            echo "  получено:  $GOT" >&2
            exit 1
        fi
        [ -n "$GOT" ] && echo "sha256 сошлась."
    fi
fi

chmod +x "$TMP/ferry"
mv "$TMP/ferry" "$DEST/ferry"

# ---- 4. Запоминаем релей ----
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/ferry"
mkdir -p "$CONFIG_DIR"
{
    echo "# Настройки клиента Ferry. Прописаны установщиком с ${RELAY_HOST}."
    echo "relay = ${RELAY_SCHEME}://${RELAY_HOST}"
    if [ "$INSECURE" = "1" ]; then
        echo "# Сертификат релея самоподписанный. Уберите эту строку, когда"
        echo "# у релея появится домен и настоящий сертификат."
        echo "insecure = true"
    fi
} > "$CONFIG_DIR/config"

# ---- 5. Что дальше ----
echo
echo "Готово: $DEST/ferry"
echo "Релей запомнен в $CONFIG_DIR/config"

case ":$PATH:" in
    *":$DEST:"*) ;;
    *)
        echo
        echo "Каталог $DEST не в PATH. Добавьте:"
        echo "  echo 'export PATH=\"\$PATH:$DEST\"' >> ~/.profile && . ~/.profile"
        ;;
esac

echo
echo "Отправить файл:"
echo "  ferry send большой-файл.tar.zst"
echo
echo "Раздача живёт, пока запущена команда — на сервере её стоит запускать в tmux:"
echo "  tmux new -s ferry"
