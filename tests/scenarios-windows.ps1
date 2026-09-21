# ============================================================
#  Ferry — сценарные проверки клиента под Windows.
#
#  Линуксовые сценарии живут в scenarios.sh; здесь проверяется ровно то,
#  чем Windows отличается, и ради чего писался слой платформы:
#
#    1) кириллица в имени файла и пробелы в пути проходят весь путь —
#       командную строку, манифест, диск на той стороне;
#    2) том, отправленный из Windows, принимает линукс-клиент;
#    3) обрыв и докачка;
#    4) отказы объясняются человеческими словами.
#
#  Что нужно заранее:
#    - релей поднят и виден по $Relay (контейнер с именем из $RelayContainer);
#    - ferry.exe лежит по пути $Ferry.
#
#  Пример:
#    docker run --rm -d --name ferry-relay -p 18080:8080 ferry-server
#    .\tests\scenarios-windows.ps1
#
#  ВАЖНО про кодировку: этот файл обязан лежать с BOM. Windows PowerShell
#  5.1 читает .ps1 без BOM в кодировке системы, и кириллица в нём
#  превращается в мусор ещё до разбора — скрипт просто не запустится.
# ============================================================
param(
    [string]$Ferry = "$env:LOCALAPPDATA\Programs\ferry\ferry.exe",
    [string]$Relay = 'http://localhost:18080',
    [string]$RelayContainer = 'ferry-relay',
    [string]$LinuxImage = 'ferry-qtbuilder',
    [string]$LinuxFerry = '/build/cli/Cli/ferry'
)

$ErrorActionPreference = 'Continue'
$ProgressPreference = 'SilentlyContinue'
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

if (-not (Test-Path $Ferry)) {
    Write-Host "не найден ferry.exe: $Ferry" -ForegroundColor Red
    Write-Host "поставьте клиент (irm $Relay/install.ps1 | iex) или укажите -Ferry"
    exit 1
}

$root = "$env:TEMP\ferry-scenarios-win"
$pass = 0; $fail = 0
function Ok($m)  { Write-Host "  [ok]   $m";   $script:pass++ }
function Bad($m) { Write-Host "  [ПРОВАЛ] $m"; $script:fail++ }

Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path "$root\папка с пробелами" | Out-Null

$linkPattern = [regex]::Escape($Relay) + '/t/([A-Za-z0-9_-]+)#([A-Za-z0-9_-]+)'

function Start-Sender($path) {
    $log = "$root\send.log"
    Remove-Item $log -ErrorAction SilentlyContinue
    # Аргумент кавычим сами: Start-Process в PowerShell 5.1 склеивает
    # -ArgumentList пробелами и НЕ кавычит, поэтому путь с пробелом
    # доехал бы до клиента обрубленным.
    $p = Start-Process -FilePath $Ferry `
        -ArgumentList @('send', ('"' + $path + '"'), '--relay', $Relay, '--no-qr') `
        -NoNewWindow -PassThru -RedirectStandardOutput $log -RedirectStandardError "$root\send.err"
    foreach ($i in 1..120) {
        Start-Sleep -Milliseconds 500
        $t = Get-Content $log -Raw -Encoding UTF8 -ErrorAction SilentlyContinue
        if ($t -match $linkPattern) {
            return @{ Proc = $p; Link = $Matches[0]; Id = $Matches[1]; Key = $Matches[2] }
        }
        if ($p.HasExited) { break }
    }
    return @{ Proc = $p; Link = $null }
}
function Stop-Sender($s) {
    if ($s -and $s.Proc -and -not $s.Proc.HasExited) { Stop-Process -Id $s.Proc.Id -Force }
    Start-Sleep -Milliseconds 500
}
function New-TestFile($path, $size, $seed) {
    $b = New-Object byte[] $size
    (New-Object System.Random $seed).NextBytes($b)
    [System.IO.File]::WriteAllBytes($path, $b)
    $b = $null
    [System.GC]::Collect()
    return (Get-FileHash $path -Algorithm SHA256).Hash
}

# ===================================================================
Write-Host "=== 1. Windows -> линукс-сервер, кириллица и пробелы ==="
$src = "$root\папка с пробелами\отчёт за квартал.bin"
$srcHash = New-TestFile $src 200MB 7

$s = Start-Sender $src
if (-not $s.Link) {
    Bad "ссылка не появилась"
    Get-Content "$root\send.log" -Encoding UTF8 -ErrorAction SilentlyContinue
} else {
    # Ссылка от Windows указывает на localhost, до которого контейнеру не
    # дотянуться. Берём её по частям: --id и --key как раз для таких
    # случаев (и заодно проверяются они сами).
    $out = docker run --rm --network "container:$RelayContainer" -v ferry-build:/build $LinuxImage `
        $LinuxFerry get --id $s.Id --key $s.Key --relay http://localhost:8080 `
        -y -o /tmp/got.bin --name "серый сервер" 2>&1
    $text = $out -join "`n"
    if ($text -match 'проверено BLAKE3') { Ok "линукс-клиент принял том из Windows" }
    else { Bad "линукс-клиент не принял"; Write-Host ($out | Select-Object -Last 6) }
    if ($text -match 'отчёт за квартал') { Ok "имя из манифеста доехало кириллицей" }
    else { Bad "имя тома потерялось" }
    Stop-Sender $s
}

# ===================================================================
Write-Host ""
Write-Host "=== 2. Обрыв и докачка ==="
# Том побольше: на петле 200 МБ уезжают быстрее, чем тест успевает
# прервать приём, и докачка осталась бы непроверенной.
$big = "$root\большой том.bin"
$bigHash = New-TestFile $big 1GB 11

$s = Start-Sender $big
if (-not $s.Link) {
    Bad "ссылка не появилась"
} else {
    $dst = "$root\с докачкой.bin"
    $g = Start-Process -FilePath $Ferry `
        -ArgumentList @('get', ('"' + $s.Link + '"'), '-y', '-o', ('"' + $dst + '"')) `
        -NoNewWindow -PassThru -RedirectStandardOutput "$root\get1.log" -RedirectStandardError "$root\get1.err"
    Start-Sleep -Milliseconds 2500
    if (-not $g.HasExited) { Stop-Process -Id $g.Id -Force }
    Start-Sleep -Milliseconds 500

    if ((Test-Path "$dst.ferry-part") -and (Test-Path "$dst.ferry-map")) {
        Ok "после обрыва остались недокачка и карта принятого"
        Stop-Sender $s
        $s2 = Start-Sender $big
        if (-not $s2.Link) {
            Bad "вторая раздача не поднялась"
        } else {
            & $Ferry get $s2.Link -y -o $dst > "$root\get2.log" 2>&1
            $log2 = Get-Content "$root\get2.log" -Raw -Encoding UTF8 -ErrorAction SilentlyContinue
            if ((Test-Path $dst) -and ((Get-FileHash $dst -Algorithm SHA256).Hash -eq $bigHash)) {
                Ok "докачка дошла до конца, хеш сошёлся"
            } else {
                Bad "докачка не сошлась"; Write-Host $log2
            }
            if ($log2 -match 'продолжаем') { Ok "клиент сказал, что продолжает" }
            else { Bad "про докачку промолчал" }
            Stop-Sender $s2
        }
    } else {
        Bad "недокачка не сохранилась"
        Get-ChildItem $root | Select-Object Name, Length
        Stop-Sender $s
    }
}

# ===================================================================
Write-Host ""
Write-Host "=== 3. Отказы объясняются ==="
$s = Start-Sender $src
if (-not $s.Link) {
    Bad "ссылка не появилась"
} else {
    $noKey = $s.Link -replace '#.*$', ''
    $o = & $Ferry get $noKey -y -o "$root\нет.bin" 2>&1 | Out-String
    if ($o -match 'нет ключа') { Ok "ссылка без ключа объяснена" } else { Bad "невнятный отказ: $o" }

    $o = & $Ferry get "$noKey#AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" -y -o "$root\чужой.bin" 2>&1 | Out-String
    if ($o -match 'не расшифровалс') { Ok "чужой ключ отвергнут на манифесте" } else { Bad "чужой ключ прошёл: $o" }
    if (Test-Path "$root\чужой.bin") { Bad "файл всё-таки создан" } else { Ok "файла на диске не осталось" }

    Stop-Sender $s
}

Write-Host ""
Write-Host "=== 4. Папка целиком ==="
# То же, что и в линуксовом сценарии, но с тем, на чём ломается именно
# Windows: разделители пути, кириллица в именах и пробелы.
#
# Пути здесь собираются Join-Path, а не склейкой через обратный слэш, и
# это не вкусовщина: в .ps1 обратный слэш перед буквой — постоянный
# источник «Illegal characters in path», а Join-Path такой возможности
# не оставляет.
$treeSrc = Join-Path $root 'дерево исходное'
$treeOut = Join-Path $root 'дерево принятое'
Remove-Item -Recurse -Force $treeSrc, $treeOut -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path (Join-Path (Join-Path $treeSrc 'документы') '2024 отчёты') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path (Join-Path $treeSrc 'code') 'nested') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $treeSrc 'пустая папка') | Out-Null

function New-RandFile($path, $size) {
    $fs = [System.IO.File]::Create($path)
    $buf = New-Object byte[] (65536)
    $rng = [System.Random]::new($size)
    $left = $size
    while ($left -gt 0) {
        $take = [Math]::Min($left, $buf.Length)
        $rng.NextBytes($buf)
        $fs.Write($buf, 0, $take)
        $left -= $take
    }
    $fs.Close()
}
New-RandFile (Join-Path (Join-Path (Join-Path $treeSrc 'документы') '2024 отчёты') 'январь.bin') 3000000
New-RandFile (Join-Path (Join-Path $treeSrc 'code') 'main.cpp') 700000
New-RandFile (Join-Path (Join-Path (Join-Path $treeSrc 'code') 'nested') 'deep.dat') 1500000
New-Item -ItemType File -Path (Join-Path $treeSrc 'пустой.txt') | Out-Null

function Get-TreeFingerprint($base) {
    $acc = ''
    foreach ($it in (Get-ChildItem -Recurse -Force $base | Sort-Object FullName)) {
        $rel = $it.FullName.Substring($base.Length)
        if ($it.PSIsContainer) { $acc += "D|$rel`n" }
        else { $acc += "F|$rel|$($it.Length)|$((Get-FileHash $it.FullName -Algorithm SHA256).Hash)`n" }
    }
    return $acc
}

$st = Start-Sender $treeSrc
if (-not $st.Link) {
    Bad "ссылка на папку не появилась"
} else {
    & $Ferry get $st.Link -y -o $treeOut 2>&1 | Out-File (Join-Path $root 'tree-get.log') -Encoding utf8
    if ($LASTEXITCODE -eq 0 -and (Get-TreeFingerprint $treeSrc) -eq (Get-TreeFingerprint $treeOut)) {
        Ok "дерево совпало байт в байт, вместе с кириллицей и пробелами"
    } else {
        Bad "дерево разошлось"
        Get-Content (Join-Path $root 'tree-get.log') -Encoding UTF8 -ErrorAction SilentlyContinue | Select-Object -Last 4
    }
    if (Test-Path (Join-Path $treeOut 'пустая папка') -PathType Container) {
        Ok "пустая папка доехала"
    } else {
        Bad "пустая папка потерялась"
    }
    if (-not (Test-Path ($treeOut + '.ferry-part')) -and -not (Test-Path ($treeOut + '.ferry-map'))) {
        Ok "временное убрано"
    } else {
        Bad "осталась недокачка"
    }
    if (-not $st.Proc.HasExited) { Stop-Process -Id $st.Proc.Id -Force }
}

Write-Host ""
Write-Host "================================"
Write-Host "прошло: $pass, провалено: $fail"
Write-Host "================================"
Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue
if ($fail -gt 0) { exit 1 } else { exit 0 }
