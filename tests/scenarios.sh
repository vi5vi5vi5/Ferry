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

    # То же самое с другой стороны — глазами получателя. Сервер шлёт ему
    # разбивку по источникам, и в итоговом кадре панели должно быть
    # видно, что большая часть приехала от пиров, а не от отправителя.
    SRC_LINE=$(grep -a 'источник окно' "$WORK/thesis-b.log" | tail -1 || true)
    echo "  $SRC_LINE"
    if [ -n "$SRC_LINE" ]; then
        ok "получатель видит разбивку по источникам"
    else
        bad "в панели получателя нет строки про источники"
    fi

    # И карта тома. У опоздавшего обе волны обязаны быть различимы
    # БЕЗ ЦВЕТА: в журнале его нет, а разницу видеть надо.
    MAP_LINE=$(grep -a 'карта' "$WORK/thesis-b.log" | tail -1 || true)
    if echo "$MAP_LINE" | grep -q '▓' && echo "$MAP_LINE" | grep -q '█'; then
        ok "на карте видны обе волны — и в оттенках серого"
    else
        bad "карта не различает волны: $MAP_LINE"
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
echo "=== 13. Папка целиком ==="
# Дерево со всем, на чём обычно ломаются: вложенность, кириллица,
# пробелы в именах, пустая папка, пустой файл и файл через границу чанка.
rm -rf tree_src tree_out
mkdir -p "tree_src/документы/2024 отчёты" tree_src/src/nested tree_src/пустая
head -c 3000000 /dev/urandom > "tree_src/документы/2024 отчёты/январь.bin"
head -c 700000  /dev/urandom > tree_src/src/main.cpp
head -c 1500000 /dev/urandom > tree_src/src/nested/deep.dat
: > tree_src/пустой.txt
echo "привет" > "tree_src/читай меня.md"

LINK=$(start_send tree_src)
if [ -z "$LINK" ]; then bad "ссылка на папку не появилась"; else
    timeout 300 $FERRY get "$LINK" -y -o tree_out > "$WORK/tree-get.log" 2>&1
    RT=$?
    if [ $RT -ne 0 ]; then
        bad "папка не приехала (код $RT)"; tail -5 "$WORK/tree-get.log"
    elif diff -r tree_src tree_out > "$WORK/tree-diff.log" 2>&1; then
        ok "дерево совпало байт в байт (diff -r)"
    else
        bad "дерево разошлось"; head -10 "$WORK/tree-diff.log"
    fi

    if [ -d "tree_out/пустая" ]; then
        ok "пустая папка доехала"
    else
        bad "пустая папка потерялась"
    fi
    if [ -f "tree_out/пустой.txt" ] && [ ! -s "tree_out/пустой.txt" ]; then
        ok "пустой файл доехал пустым"
    else
        bad "пустой файл потерялся"
    fi
    # Недокачки и карты после успешного приёма оставаться не должны.
    if [ ! -e tree_out.ferry-part ] && [ ! -e tree_out.ferry-map ]; then
        ok "временное убрано"
    else
        bad "осталась недокачка или карта"
    fi
fi
stop_send

# И докачка дерева: недокачка здесь — КАТАЛОГ, а не файл, и это
# отдельный путь в коде.
rm -rf tree_src tree_out tree_out.ferry-part tree_out.ferry-map
# Два гигабайта из /dev/zero, а не из /dev/urandom, и это не лень: том
# нужен такой, чтобы передача точно не успела закончиться до обрыва,
# а случайных данных столько генерируется дольше, чем весь тест.
# Раскладку по файлам проверяет первая половина сценария, где данные
# случайные, а здесь проверяется докачка.
mkdir -p tree_src/a tree_src/b
head -c 1073741824 /dev/zero > tree_src/a/big1.bin
head -c 1073741824 /dev/zero > tree_src/b/big2.bin
LINK=$(start_send tree_src)
if [ -z "$LINK" ]; then bad "ссылка не появилась"; else
    timeout 300 $FERRY get "$LINK" -y -o tree_out > "$WORK/tree-r1.log" 2>&1 &
    GP=$!
    sleep 2
    kill -INT $GP 2>/dev/null || true
    wait $GP 2>/dev/null || true

    if [ -d tree_out.ferry-part ] && [ -f tree_out.ferry-map ]; then
        ok "после обрыва остались каталог недокачки и карта"
    else
        bad "недокачка дерева не сохранилась"
    fi

    timeout 300 $FERRY get "$LINK" -y -o tree_out > "$WORK/tree-r2.log" 2>&1
    if diff -r tree_src tree_out > /dev/null 2>&1; then
        ok "докачка дерева дошла до конца"
    else
        bad "после докачки дерево разошлось"; tail -4 "$WORK/tree-r2.log"
    fi
    if grep -qa 'продолжаем' "$WORK/tree-r2.log"; then
        ok "клиент сказал, что продолжает"
    else
        bad "про продолжение ничего не сказал"
    fi
fi
stop_send
rm -rf tree_src tree_out tree_out.ferry-part tree_out.ferry-map

echo
echo ""
echo "=== 15. Что в том не берётся, о том сказано вслух ==="
# Симлинки и не-файлы (FIFO, сокеты, устройства) в том не едут. Это
# правильно, но молча так делать нельзя: человек отправит папку, получит
# на той стороне неполное дерево и никогда не узнает почему. Проверяем
# именно то, что отправитель СКАЗАЛ, а не только то, что он пропустил.
rm -rf skip_src skip_out
mkdir -p skip_src/внутри
echo "настоящий файл" > skip_src/внутри/real.txt
head -c 200000 /dev/urandom > skip_src/данные.bin
ln -s /etc/hostname skip_src/ссылка-на-файл
ln -s /tmp skip_src/ссылка-на-папку
mkfifo skip_src/труба

LINK=$(start_send skip_src)
if [ -z "$LINK" ]; then bad "ссылка не появилась"; else
    if grep -q "пропущено" "$WORK/send.log"; then
        ok "отправитель предупредил о пропущенном"
    else
        bad "о пропущенном промолчали"; tail -12 "$WORK/send.log"
    fi
    if grep -q "ссылка-на-файл" "$WORK/send.log" \
       && grep -q "ссылка-на-папку" "$WORK/send.log"; then
        ok "обе ссылки названы поимённо"
    else
        bad "ссылки не названы"; tail -12 "$WORK/send.log"
    fi
    if grep -q "труба" "$WORK/send.log"; then
        ok "FIFO не попал в том и назван"
    else
        bad "FIFO не назван — а он либо в томе, либо потерян молча"
        tail -12 "$WORK/send.log"
    fi
    # Числительные согласованы: здесь ровно два файла и две записи
    # пропущено (две ссылки и труба — три). «2 файлов» в готовом
    # продукте выглядит как недоделанный перевод.
    if grep -q "2 файла" "$WORK/send.log"        && grep -q "3 записи" "$WORK/send.log"; then
        ok "числительные согласованы со словами"
    else
        bad "числительные не согласованы"
        grep -E "состав|пропущено" "$WORK/send.log"
    fi

    timeout 300 $FERRY get "$LINK" -y -o skip_out > "$WORK/skip-get.log" 2>&1
    if [ $? -ne 0 ]; then
        bad "том с пропусками не приехал"; tail -5 "$WORK/skip-get.log"
    elif [ -f skip_out/внутри/real.txt ] \
         && [ -f skip_out/данные.bin ] \
         && [ ! -e skip_out/ссылка-на-файл ] \
         && [ ! -e skip_out/труба ]; then
        ok "приехало ровно то, что обещали: файлы есть, пропущенного нет"
    else
        bad "состав принятого дерева не тот"; ls -la skip_out
    fi
    # Самое важное: то, что доехало, должно совпасть побайтно.
    if cmp -s skip_src/данные.bin skip_out/данные.bin; then
        ok "оставшиеся файлы целы"
    else
        bad "файлы побились"
    fi
fi
stop_send

echo ""
echo "=== 16. Хеши на лету: ссылка раньше, чем посчитан том ==="
# Раньше отправитель читал том целиком ДО ссылки, и архив на сто гигабайт
# держал человека у полосы «считаю хеши» по десять минут. Теперь хеши
# считаются в фоне, а раздача начинается сразу. Чтобы это было видно на
# быстром диске, подсчёт здесь искусственно замедлен до 50 МБ/с: том в
# 500 МиБ считается около десяти секунд, а ссылка обязана появиться сильно
# раньше.
head -c 524288000 /dev/urandom > stream.bin
H16=$(sha256sum stream.bin | cut -d' ' -f1)
export FERRY_TEST_HASH_MBPS=50
T0=$(date +%s%N)
LINK=$(start_send stream.bin)
T1=$(date +%s%N)
unset FERRY_TEST_HASH_MBPS
if [ -z "$LINK" ]; then bad "ссылка не появилась"; else
    MS=$(( (T1 - T0) / 1000000 ))
    if [ "$MS" -lt 5000 ]; then
        ok "ссылка через $MS мс — задолго до конца подсчёта (около 10 с)"
    else
        bad "ссылка появилась только через $MS мс — хеши считались заранее?"
    fi

    ID=$(echo "$LINK" | sed 's|.*/t/||; s|#.*||')
    # Старый клиент заголовка не пришлёт — ему честное «подождите».
    CODE=$(curl -s -o "$WORK/meta-old.json" -w '%{http_code}' \
                "http://localhost:8080/api/transfers/$ID")
    if [ "$CODE" = "409" ] && [ "$(jq -r .error "$WORK/meta-old.json")" = "preparing" ]; then
        ok "клиент без хешей на лету получает «подождите», а не сломанный том"
    else
        bad "старому клиенту ответили $CODE: $(cat "$WORK/meta-old.json")"
    fi
    # Новому — промежуточный манифест без списка.
    MODE=$(curl -s -H 'X-Ferry-Features: stream_hashes' \
                "http://localhost:8080/api/transfers/$ID" | jq -r .hash_mode)
    if [ "$MODE" = "stream" ]; then
        ok "новому клиенту — промежуточный манифест, хеши сегментами"
    else
        bad "hash_mode=$MODE"
    fi

    # Получатель стартует, пока хеши ещё считаются.
    timeout 300 $FERRY get "$LINK" -y -o stream.out > "$WORK/get16.log" 2>&1
    R16=$?
    if [ $R16 -eq 0 ] && [ "$(sha256sum stream.out | cut -d' ' -f1)" = "$H16" ]; then
        ok "получатель, пришедший во время подсчёта, забрал том целым"
    else
        bad "том не сошёлся (код $R16)"; tail -5 "$WORK/get16.log"
    fi

    # Досчитано — и раздача открыта всем, в том числе старым клиентам.
    CODE=$(curl -s -o "$WORK/meta-after.json" -w '%{http_code}' \
                "http://localhost:8080/api/transfers/$ID")
    if [ "$CODE" = "200" ] \
       && [ "$(jq -r '.hash_list | length' "$WORK/meta-after.json")" -gt 0 ]; then
        ok "после подсчёта раздача обычная: полный список отдаётся и старым клиентам"
    else
        bad "после подсчёта ответ $CODE"
    fi
fi
stop_send
if grep -q "хеши посчитаны на лету" "$WORK/send.log"; then
    ok "отправитель сказал, что считал хеши на лету"
else
    bad "в выводе отправителя нет строки про хеши на лету"; tail -8 "$WORK/send.log"
fi
rm -f stream.bin stream.out

echo ""
echo "=== 17. Нечитаемый файл виден до ссылки, а не роняет раздачу ==="
# С файлового сервера раздача падала посередине на архиве, который не
# читался, и говорила только «файл перестал читаться». Теперь такой файл
# выясняется при описи и попадает в список пропущенного — с причиной от
# системы. Отправитель здесь не root: root права на чтение не проверяет.
rm -rf perm_src
mkdir -p perm_src
echo "обычный" > perm_src/a.txt
head -c 3000000 /dev/urandom > perm_src/закрытый.zip
chmod -R a+rX perm_src
chmod 000 perm_src/закрытый.zip
OUT=$(runuser -u nobody -- $FERRY send perm_src --relay http://127.0.0.1:9 --no-qr 2>&1 || true)
if echo "$OUT" | grep -q "закрытый.zip (не читается: Permission denied"; then
    ok "нечитаемый архив назван до ссылки, с причиной от системы"
else
    bad "про нечитаемый архив не сказано"; echo "$OUT" | tail -8
fi
if echo "$OUT" | grep -q "1 файл$\|1 файл "; then
    ok "в томе остался только читаемый файл"
else
    bad "состав тома не тот"; echo "$OUT" | grep 'состав'
fi
rm -rf perm_src

echo "=== 14. ferry uninstall убирает за собой ==="
# Ставим копию в песочницу и смотрим, что осталось после неё.
UNI="$WORK/uninstall"
rm -rf "$UNI"
mkdir -p "$UNI/bin" "$UNI/home"
cp "$FERRY" "$UNI/bin/ferry"
XDG_CONFIG_HOME="$UNI/home" "$UNI/bin/ferry" config > /dev/null 2>&1 || true
mkdir -p "$UNI/home/ferry"
printf 'relay = http://localhost:8080\n' > "$UNI/home/ferry/config"

XDG_CONFIG_HOME="$UNI/home" "$UNI/bin/ferry" uninstall -y > "$WORK/uninstall.log" 2>&1
RU=$?
if [ $RU -eq 0 ]; then
    ok "деинсталляция отработала без ошибок"
else
    bad "деинсталляция вернула $RU"; tail -5 "$WORK/uninstall.log"
fi
if [ ! -e "$UNI/bin/ferry" ]; then
    ok "бинарь удалён"
else
    bad "бинарь остался на месте"
fi
if [ ! -e "$UNI/home/ferry" ]; then
    ok "каталог настроек удалён"
else
    bad "настройки остались: $(ls -A "$UNI/home/ferry")"
fi
rm -rf "$UNI"

echo
echo "=== 8. Сервер по-прежнему ничего не хранит ==="
curl -s localhost:8080/api/health | jq -c '{transfers, window_bytes_used, stores_on_disk}'
echo "в веб-корне релея:"; ls -A "$WORK/web" | wc -l | sed "s/^/  файлов: /"

echo
echo "================================"
echo "прошло: $PASS, провалено: $FAIL"
echo "================================"
[ $FAIL -eq 0 ]
