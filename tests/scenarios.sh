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
echo "=== 9. Опоздавший: начало тома давно вытеснено из окна ==="
# Второй релей с крошечным окном: 8 МиБ против тома в 200. Окно — буфер
# джиттера, и чтобы это было видно в тесте, его надо сделать маленьким:
# с окном по умолчанию том целиком влезает в оперативку, и опоздать не куда.
mkdir -p "$WORK/late"
printf '[log]
level = normal
' > "$WORK/late/ferry.conf"
FERRY_TRANSFER_WINDOW_MB=8 $SERVER --http-port 8081 --ws-port 9001     --web-root "$WORK/late-web" --config-dir "$WORK/late" > "$WORK/late-server.log" 2>&1 &
LATE_PID=$!
sleep 1

head -c 209715200 /dev/urandom > late.bin
H9=$(sha256sum late.bin | cut -d' ' -f1)
$FERRY send late.bin --relay http://localhost:8081 --no-qr > "$WORK/late-send.log" 2>&1 &
LATE_SEND=$!
LINK=""
for i in $(seq 1 90); do
    LINK=$(grep -oE 'http://localhost:8081/t/[A-Za-z0-9_-]+#[A-Za-z0-9_-]+' "$WORK/late-send.log" | head -1 || true)
    [ -n "$LINK" ] && break
    sleep 1
done

if [ -z "$LINK" ]; then bad "ссылка не появилась"; else
    # Первый забирает том ЦЕЛИКОМ и уходит. После этого в окне лежат
    # последние восемь мегабайт, а начала тома нет ни у кого, кроме
    # отправителя. Это и есть случай, на котором M1 отвечал no_source.
    timeout 300 $FERRY get "$LINK" -y -o late1.out > "$WORK/late-get1.log" 2>&1
    R1=$?
    timeout 300 $FERRY get "$LINK" -y -o late2.out > "$WORK/late-get2.log" 2>&1
    R2=$?

    if [ $R1 -eq 0 ] && [ "$(sha256sum late1.out | cut -d' ' -f1)" = "$H9" ]; then
        ok "первый получатель забрал том целиком"
    else
        bad "первый получатель не справился (код $R1)"; tail -4 "$WORK/late-get1.log"
    fi

    if grep -qi 'no_source\|нет ни отправителя' "$WORK/late-get2.log"; then
        bad "опоздавший получил отказ no_source"
    else
        ok "опоздавшему не отказали"
    fi
    if [ $R2 -eq 0 ] && [ "$(sha256sum late2.out | cut -d' ' -f1)" = "$H9" ]; then
        ok "опоздавший доехал второй волной, хеш сошёлся"
    else
        bad "опоздавший не доехал (код $R2)"; tail -6 "$WORK/late-get2.log"
    fi
fi
kill $LATE_SEND 2>/dev/null || true
kill $LATE_PID 2>/dev/null || true
rm -f late.bin late1.out late2.out
sleep 1

echo
echo "=== 10. Тезис: опоздавшие берут начало у сида, а не у отправителя ==="
# Главная проверка всего M2. Тот же релей с окном в 8 МиБ.
#
# Первый получатель забирает том и ОСТАЁТСЯ (--seed). Двое следующих
# приходят, когда начала тома в окне давно нет. Отправитель при этом
# обязан отдать том ОДИН раз — это и есть обещание продукта.
# Двое опоздавших, а не один, нарочно: так проверяется ещё и
# коалесцирование — один чанк, нужный обоим, спрашивается один раз.
mkdir -p "$WORK/thesis"
printf '[log]
level = normal
' > "$WORK/thesis/ferry.conf"
FERRY_TRANSFER_WINDOW_MB=8 $SERVER --http-port 8082 --ws-port 9002     --web-root "$WORK/thesis-web" --config-dir "$WORK/thesis" > "$WORK/thesis-server.log" 2>&1 &
TH_PID=$!
sleep 1

head -c 209715200 /dev/urandom > thesis.bin
H10=$(sha256sum thesis.bin | cut -d' ' -f1)
$FERRY send thesis.bin --relay http://localhost:8082 --no-qr > "$WORK/thesis-send.log" 2>&1 &
TH_SEND=$!
LINK=""
for i in $(seq 1 90); do
    LINK=$(grep -oE 'http://localhost:8082/t/[A-Za-z0-9_-]+#[A-Za-z0-9_-]+' "$WORK/thesis-send.log" | head -1 || true)
    [ -n "$LINK" ] && break
    sleep 1
done

if [ -z "$LINK" ]; then bad "ссылка не появилась"; else
    # Сид: забирает том и остаётся источником.
    timeout 300 $FERRY get "$LINK" -y --seed -o seed.out --name Сид > "$WORK/thesis-seed.log" 2>&1 &
    SEEDER=$!
    for i in $(seq 1 300); do
        [ -f seed.out ] && [ "$(stat -c %s seed.out 2>/dev/null || echo 0)" = "209715200" ] && break
        sleep 1
    done
    sleep 2

    timeout 300 $FERRY get "$LINK" -y -o late_b.out > "$WORK/thesis-b.log" 2>&1 &
    PB=$!
    timeout 300 $FERRY get "$LINK" -y -o late_c.out > "$WORK/thesis-c.log" 2>&1 &
    PC=$!
    wait $PB; RB=$?
    wait $PC; RC=$?

    if [ $RB -eq 0 ] && [ $RC -eq 0 ]        && [ "$(sha256sum late_b.out | cut -d' ' -f1)" = "$H10" ]        && [ "$(sha256sum late_c.out | cut -d' ' -f1)" = "$H10" ]; then
        ok "оба опоздавших забрали том целиком"
    else
        bad "опоздавшие не справились (коды $RB/$RC)"
        tail -4 "$WORK/thesis-b.log"; tail -4 "$WORK/thesis-c.log"
    fi

    # Ставим точку и читаем итог отправителя.
    kill -INT $SEEDER 2>/dev/null || true
    kill -INT $TH_SEND 2>/dev/null || true
    wait $TH_SEND 2>/dev/null || true
    sleep 1

    TOTAL_LINE=$(grep -a 'всего в сеть' "$WORK/thesis-send.log" | tail -1 || true)
    REPEAT_LINE=$(grep -a 'повторно' "$WORK/thesis-send.log" | tail -1 || true)
    echo "  $TOTAL_LINE"
    echo "  $REPEAT_LINE"
    if echo "$REPEAT_LINE" | grep -q 'ничего'; then
        ok "отправитель отдал том ровно один раз — начало взяли у сида"
    else
        bad "отправителю пришлось досылать: $REPEAT_LINE"
    fi
fi
kill $SEEDER $TH_SEND 2>/dev/null || true
kill $TH_PID 2>/dev/null || true
rm -f thesis.bin seed.out late_b.out late_c.out
sleep 1

echo
echo "=== 12. Единственный сид отваливается посреди догона ==="
# Вторая волна обязана падать назад к отправителю, а не вставать насмерть
# с четырьмя повисшими просьбами к ушедшему.
mkdir -p "$WORK/gone"
printf '[log]
level = normal
' > "$WORK/gone/ferry.conf"
FERRY_TRANSFER_WINDOW_MB=8 $SERVER --http-port 8083 --ws-port 9003     --web-root "$WORK/gone-web" --config-dir "$WORK/gone" > "$WORK/gone-server.log" 2>&1 &
GONE_PID=$!
sleep 1

head -c 209715200 /dev/urandom > gone.bin
H12=$(sha256sum gone.bin | cut -d' ' -f1)
$FERRY send gone.bin --relay http://localhost:8083 --no-qr > "$WORK/gone-send.log" 2>&1 &
GONE_SEND=$!
LINK=""
for i in $(seq 1 90); do
    LINK=$(grep -oE 'http://localhost:8083/t/[A-Za-z0-9_-]+#[A-Za-z0-9_-]+' "$WORK/gone-send.log" | head -1 || true)
    [ -n "$LINK" ] && break
    sleep 1
done

if [ -z "$LINK" ]; then bad "ссылка не появилась"; else
    timeout 300 $FERRY get "$LINK" -y --seed -o gone_seed.out > "$WORK/gone-seed.log" 2>&1 &
    GSEED=$!
    # Снимаем с учёта заданий: его сейчас убьют нарочно, и «Killed» в
    # выводе теста выглядел бы поломкой.
    disown $GSEED 2>/dev/null || true
    for i in $(seq 1 300); do
        [ -f gone_seed.out ] && [ "$(stat -c %s gone_seed.out 2>/dev/null || echo 0)" = "209715200" ] && break
        sleep 1
    done
    sleep 2

    timeout 300 $FERRY get "$LINK" -y -o gone_late.out > "$WORK/gone-late.log" 2>&1 &
    PL=$!
    # Даём догону начаться — и убиваем единственный источник.
    sleep 1
    kill -9 $GSEED 2>/dev/null || true

    wait $PL; RL=$?
    if [ $RL -eq 0 ] && [ "$(sha256sum gone_late.out | cut -d' ' -f1)" = "$H12" ]; then
        ok "опоздавший доехал через отправителя, когда сид исчез"
    else
        bad "после ухода сида догон встал (код $RL)"; tail -6 "$WORK/gone-late.log"
    fi
fi
kill $GSEED $GONE_SEND 2>/dev/null || true
kill $GONE_PID 2>/dev/null || true
rm -f gone.bin gone_seed.out gone_late.out
sleep 1

echo
echo "=== 8. Сервер по-прежнему ничего не хранит ==="
curl -s localhost:8080/api/health | jq -c '{transfers, window_bytes_used, stores_on_disk}'
echo "в веб-корне релея:"; ls -A "$WORK/web" | wc -l | sed "s/^/  файлов: /"

echo
echo "================================"
echo "прошло: $PASS, провалено: $FAIL"
echo "================================"
[ $FAIL -eq 0 ]
