#!/usr/bin/env bash
# ============================================================
#  Ferry — сценарные проверки: релей и два клиента на одной машине.
#
#  Самотесты ядра (ferry-core-test) проверяют форматы и криптографию;
#  здесь проверяется то, что из них складывается: реальный перевоз,
#  веер на двоих, отказы, одноразовая ссылка и докачка после обрыва.
#
#  Запуск (после обычной сборки):
#      cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
#      cmake -S Server -B build-server -DCMAKE_BUILD_TYPE=Release && cmake --build build-server
#      ./tests/scenarios.sh
#
#  Нужны: curl, jq, sha256sum. Около 7 ГБ свободного места под /tmp:
#  сценарий с докачкой берёт том на 5 ГиБ, и это не прихоть — на меньшем
#  томе перевоз успевает закончиться раньше, чем тест успевает его
#  прервать, и докачка остаётся непроверенной.
#
#  Пути к бинарям можно задать снаружи:
#      FERRY=... SERVER=... ./tests/scenarios.sh
# ============================================================
set -u

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FERRY="${FERRY:-$REPO_ROOT/build/Cli/ferry}"
SERVER="${SERVER:-$REPO_ROOT/build-server/FerryServer}"

for bin in "$FERRY" "$SERVER"; do
    if [ ! -x "$bin" ]; then
        echo "не найден: $bin" >&2
        echo "соберите проект или укажите путь через FERRY=/SERVER=" >&2
        exit 1
    fi
done

WORK="${WORK:-/tmp/ferry-scenarios}"
PASS=0
FAIL=0

ok()   { echo "  [ok]   $1"; PASS=$((PASS+1)); }
bad()  { echo "  [ПРОВАЛ] $1"; FAIL=$((FAIL+1)); }


rm -rf "$WORK"; mkdir -p "$WORK/web" "$WORK/run"; cd "$WORK/run"
printf '[log]\nlevel = normal\n' > "$WORK/ferry.conf"
$SERVER --http-port 8080 --ws-port 9000 --web-root "$WORK/web" --config-dir "$WORK" > "$WORK/server.log" 2>&1 &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null' EXIT
sleep 1

start_send() {   # $1 = файл, $2 = доп.аргументы; печатает ссылку
    local f="$1"; shift
    $FERRY send "$f" --relay http://localhost:8080 --no-qr "$@" > "$WORK/send.log" 2>&1 &
    echo $! > "$WORK/send.pid"
    local link=""
    for i in $(seq 1 90); do
        link=$(grep -oE 'http://localhost:8080/t/[A-Za-z0-9_-]+#[A-Za-z0-9_-]+' "$WORK/send.log" | head -1 || true)
        [ -n "$link" ] && break
        sleep 1
    done
    echo "$link"
}
stop_send() { kill "$(cat "$WORK/send.pid" 2>/dev/null)" 2>/dev/null || true; sleep 1; }

echo "=== 1. Том на 1 ГиБ, один получатель ==="
head -c 1073741824 /dev/urandom > gb.bin
H1=$(sha256sum gb.bin | cut -d' ' -f1)
LINK=$(start_send gb.bin)
if [ -z "$LINK" ]; then bad "ссылка не появилась"; else
    T0=$(date +%s)
    timeout 300 $FERRY get "$LINK" -y -o gb.out >"$WORK/get1.log" 2>&1
    T1=$(date +%s)
    if [ -f gb.out ] && [ "$(sha256sum gb.out | cut -d' ' -f1)" = "$H1" ]; then
        ok "1 ГиБ приехал целым за $((T1-T0)) с"
    else
        bad "1 ГиБ не сошёлся"; tail -5 "$WORK/get1.log"
    fi
fi
stop_send
rm -f gb.out

echo
echo "=== 2. Веер: двое качают одновременно ==="
head -c 209715200 /dev/urandom > fan.bin
H2=$(sha256sum fan.bin | cut -d' ' -f1)
LINK=$(start_send fan.bin)
if [ -z "$LINK" ]; then bad "ссылка не появилась"; else
    timeout 180 $FERRY get "$LINK" -y -o fan1.out --name Костя >"$WORK/getA.log" 2>&1 &
    PA=$!
    timeout 180 $FERRY get "$LINK" -y -o fan2.out --name Марк >"$WORK/getB.log" 2>&1 &
    PB=$!
    wait $PA; RA=$?
    wait $PB; RB=$?
    if [ $RA -eq 0 ] && [ $RB -eq 0 ] \
       && [ "$(sha256sum fan1.out | cut -d' ' -f1)" = "$H2" ] \
       && [ "$(sha256sum fan2.out | cut -d' ' -f1)" = "$H2" ]; then
        ok "оба получателя забрали одинаковый том"
    else
        bad "веер не сошёлся (коды $RA/$RB)"; tail -4 "$WORK/getA.log"; tail -4 "$WORK/getB.log"
    fi
fi
stop_send
rm -f fan1.out fan2.out

echo
echo "=== 3. Чужой ключ отвергается ==="
LINK=$(start_send fan.bin)
if [ -z "$LINK" ]; then bad "ссылка не появилась"; else
    BASE="${LINK%%#*}"
    WRONG="${BASE}#AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
    OUT=$(timeout 60 $FERRY get "$WRONG" -y -o wrong.out 2>&1 || true)
    if echo "$OUT" | grep -q "не расшифровалс"; then
        ok "манифест с чужим ключом не расшифровался"
    else
        bad "чужой ключ прошёл"; echo "$OUT" | head -3
    fi
    [ -f wrong.out ] && bad "файл всё-таки создан" || ok "файла на диске не осталось"
fi
stop_send

echo
echo "=== 4. Ссылка без ключа ==="
LINK=$(start_send fan.bin)
if [ -z "$LINK" ]; then bad "ссылка не появилась"; else
    OUT=$(timeout 60 $FERRY get "${LINK%%#*}" -y -o nokey.out 2>&1 || true)
    if echo "$OUT" | grep -q "нет ключа"; then ok "клиент объяснил, что ключа нет"; else bad "невнятный отказ"; echo "$OUT" | head -3; fi
fi
stop_send

echo
echo "=== 5. Одноразовая ссылка ==="
LINK=$(start_send fan.bin --uses 1)
if [ -z "$LINK" ]; then bad "ссылка не появилась"; else
    timeout 120 $FERRY get "$LINK" -y -o once.out >/dev/null 2>&1 && ok "первый получатель забрал" || bad "первый не смог"
    OUT=$(timeout 60 $FERRY get "$LINK" -y -o once2.out 2>&1 || true)
    if echo "$OUT" | grep -q "столько раз"; then ok "второй получил отказ по исчерпанию"; else bad "лимит не сработал"; echo "$OUT" | head -3; fi
fi
stop_send
rm -f once.out once2.out

echo
echo "=== 6. Превью-бот не жжёт использование ==="
LINK=$(start_send fan.bin --uses 1)
if [ -z "$LINK" ]; then bad "ссылка не появилась"; else
    ID=$(echo "$LINK" | sed 's|.*/t/||; s|#.*||')
    for i in 1 2 3; do curl -s "http://localhost:8080/api/transfers/$ID" >/dev/null; done
    LEFT=$(curl -s "http://localhost:8080/api/transfers/$ID" | jq -r .uses_left)
    if [ "$LEFT" = "1" ]; then ok "четыре обращения к метаданным — использование на месте"; else bad "uses_left=$LEFT"; fi
fi
stop_send

echo
echo "=== 7. Том на 5 ГиБ: обрыв и докачка ==="
head -c 5368709120 /dev/urandom > res.bin
H7=$(sha256sum res.bin | cut -d' ' -f1)
LINK=$(start_send res.bin)
if [ -z "$LINK" ]; then bad "ссылка не появилась"; else
    timeout 600 $FERRY get "$LINK" -y -o res.out >"$WORK/get7.log" 2>&1 &
    GP=$!
    sleep 6
    kill -INT $GP 2>/dev/null || true
    wait $GP 2>/dev/null || true
    if [ -f res.out.ferry-part ] && [ -f res.out.ferry-map ]; then
        PARTIAL=$(stat -c%s res.out.ferry-part)
        ok "после Ctrl-C осталась недокачка и карта принятого"
        # Отправитель перезапускается: новая раздача, тот же файл.
        stop_send
        LINK2=$(start_send res.bin)
        if timeout 600 $FERRY get "$LINK2" -y -o res.out >"$WORK/get7b.log" 2>&1 \
           && [ "$(sha256sum res.out | cut -d' ' -f1)" = "$H7" ]; then
            ok "докачка дошла до конца, хеш сошёлся"
            grep -q "продолжаем" "$WORK/get7b.log" && ok "клиент сказал, что продолжает" || bad "про докачку промолчал"
        else
            bad "докачка не сошлась"; tail -5 "$WORK/get7b.log"
        fi
    else
        bad "недокачка не сохранилась"
    fi
fi
stop_send

echo
echo "=== 8. Сервер по-прежнему ничего не хранит ==="
curl -s localhost:8080/api/health | jq -c '{transfers, window_bytes_used, stores_on_disk}'
echo "в веб-корне релея:"; ls -A "$WORK/web" | wc -l | sed "s/^/  файлов: /"

echo
echo "================================"
echo "прошло: $PASS, провалено: $FAIL"
echo "================================"
[ $FAIL -eq 0 ]
