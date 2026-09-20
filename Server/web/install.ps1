# ============================================================
#  Ferry — установка клиента на Windows.
#
#      irm https://<релей>/install.ps1 | iex
#
#  Скрипт отдаёт сам релей, и адрес релея он подставляет сюда на лету —
#  поэтому после установки `ferry send файл` работает без единого флага.
#
#  Что происходит: скачивается один .exe, сверяется его sha256, файл
#  кладётся в %LOCALAPPDATA%\Programs\ferry, этот каталог добавляется в
#  PATH пользователя, адрес релея пишется в %APPDATA%\ferry\config.
#  Ни служб, ни записей в реестре, ни прав администратора.
#
#  Удалить:
#      Remove-Item -Recurse "$env:LOCALAPPDATA\Programs\ferry"
#      Remove-Item -Recurse "$env:APPDATA\ferry"
#      (и убрать каталог из PATH в «Переменные среды»)
#
#  Сертификат релея самоподписанный? Тогда так:
#      $env:FERRY_INSECURE = '1'
#      [Net.ServicePointManager]::ServerCertificateValidationCallback = { $true }
#      irm https://<релей>/install.ps1 | iex
# ============================================================
param(
    [switch]$Insecure,
    [string]$Prefix
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$RelayHost   = '@FERRY_RELAY@'
$RelayScheme = '@FERRY_SCHEME@'
$Base = "${RelayScheme}://${RelayHost}"

# Через `irm | iex` параметры не передать: скрипт приходит строкой, а не
# файлом. Поэтому тот же выключатель читается и из окружения.
if ($env:FERRY_INSECURE -in @('1', 'true', 'yes', 'on')) { $Insecure = $true }

# Windows PowerShell 5.1 по умолчанию ходит по TLS 1.0, который релей не
# примет. Строка ниже касается только этого сеанса.
try {
    [Net.ServicePointManager]::SecurityProtocol =
        [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
} catch { }

if ($Insecure) {
    Write-Host "ВНИМАНИЕ: сертификат релея не проверяется." -ForegroundColor Yellow
    Write-Host "Так бывает, пока у релея нет домена. Посредник в сети может выдать"
    Write-Host "себя за него. Когда домен появится, это станет не нужно."
    Write-Host ""
    try {
        [Net.ServicePointManager]::ServerCertificateValidationCallback = { $true }
    } catch { }
}

# ---- 1. Архитектура ----
$arch = $env:PROCESSOR_ARCHITECTURE
if ($env:PROCESSOR_ARCHITEW6432) { $arch = $env:PROCESSOR_ARCHITEW6432 }
switch ($arch) {
    'AMD64' { $asset = 'ferry-windows-x86_64.exe' }
    'ARM64' {
        # На Windows ARM64 x86_64-бинарь запускается через эмуляцию: она
        # медленнее, но работает, и это честнее, чем отказать.
        $asset = 'ferry-windows-x86_64.exe'
        Write-Host "Сборки под ARM64 пока нет — ставим x86_64, он пойдёт через эмуляцию." -ForegroundColor Yellow
    }
    default {
        Write-Host "Неизвестная архитектура: $arch" -ForegroundColor Red
        Write-Host "Соберите из исходников: https://github.com/vi5vi5vi5/Ferry"
        return
    }
}

# ---- 2. Куда класть ----
if ($Prefix) { $dest = $Prefix } else { $dest = "$env:LOCALAPPDATA\Programs\ferry" }
New-Item -ItemType Directory -Force -Path $dest | Out-Null

# ---- 3. Скачиваем и сверяем ----
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("ferry-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    Write-Host "Скачиваю $asset с $RelayHost…"
    $tmpExe = Join-Path $tmp 'ferry.exe'
    try {
        Invoke-WebRequest -Uri "$Base/dl/$asset" -OutFile $tmpExe -UseBasicParsing
    } catch {
        # Без этого человек получал бы стек PowerShell вместо ответа на
        # вопрос «а что мне теперь делать».
        Write-Host "Не удалось скачать $asset с $RelayHost." -ForegroundColor Red
        Write-Host "  $($_.Exception.Message)"
        Write-Host ""
        Write-Host "Если ответ 404 — на релее нет сборки под Windows. Обновите релей:"
        Write-Host "  cd Ferry/Server && ./tools/update.sh --force"
        Write-Host "Если ошибка про сертификат — у релея его ещё нет, попробуйте так:"
        Write-Host "  `$env:FERRY_INSECURE = '1'"
        Write-Host "  [Net.ServicePointManager]::ServerCertificateValidationCallback = { `$true }"
        Write-Host "  irm $Base/install.ps1 | iex"
        return
    }

    # Сверка обязательна: это файл, который человек сейчас запустит.
    #
    # Тело читаем с явным декодированием, а не берём .Content как строку.
    # Windows PowerShell 5.1 решает сам, отдать строку или Byte[], и для
    # нашего text/plain отдаёт именно байты — а `$bytes -split` молча
    # разваливает их на отдельные числа, из-за чего проверка тихо не
    # находила нужной строки и тихо ничего не проверяла. Молчаливо
    # пропущенная сверка хуже отсутствующей: она создаёт уверенность.
    $sums = $null
    try {
        $resp = Invoke-WebRequest -Uri "$Base/dl/SHA256SUMS" -UseBasicParsing
        if ($resp.Content -is [byte[]]) {
            $sums = [System.Text.Encoding]::UTF8.GetString($resp.Content)
        } else {
            $sums = [string]$resp.Content
        }
    } catch { }

    if (-not $sums) {
        Write-Host "Не удалось получить список контрольных сумм — ставим без сверки." -ForegroundColor Yellow
    } else {
        $want = $null
        foreach ($line in ($sums -split "`r?`n")) {
            $parts = ($line.Trim() -split '\s+')
            if ($parts.Count -ge 2 -and $parts[-1] -eq $asset) { $want = $parts[0].ToLower() }
        }
        if (-not $want) {
            Write-Host "В списке сумм нет строки про $asset — это не то, чего мы ждали." -ForegroundColor Red
            return
        }
        $got = (Get-FileHash $tmpExe -Algorithm SHA256).Hash.ToLower()
        if ($got -ne $want) {
            Write-Host "Контрольная сумма не сошлась — файл по дороге подменили или побился." -ForegroundColor Red
            Write-Host "  ожидалось: $want"
            Write-Host "  получено:  $got"
            return
        }
        Write-Host "sha256 сошлась."
    }

    $target = Join-Path $dest 'ferry.exe'
    Move-Item -Force $tmpExe $target
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

# ---- 4. Запоминаем релей ----
$configDir = "$env:APPDATA\ferry"
New-Item -ItemType Directory -Force -Path $configDir | Out-Null
$lines = @(
    "# Настройки клиента Ferry. Прописаны установщиком с $RelayHost.",
    "relay = ${RelayScheme}://${RelayHost}"
)
if ($Insecure) {
    $lines += "# Сертификат релея самоподписанный. Уберите эту строку, когда"
    $lines += "# у релея появится домен и настоящий сертификат."
    $lines += "insecure = true"
}
# Без BOM и в UTF-8: файл читает наш же клиент, и он ждёт UTF-8.
[System.IO.File]::WriteAllLines("$configDir\config", $lines,
    (New-Object System.Text.UTF8Encoding $false))

# ---- 5. PATH ----
$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if (-not $userPath) { $userPath = '' }
$already = ($userPath -split ';' | Where-Object { $_.TrimEnd('\') -ieq $dest.TrimEnd('\') })
if (-not $already) {
    [Environment]::SetEnvironmentVariable('Path', ($userPath.TrimEnd(';') + ';' + $dest).TrimStart(';'), 'User')
    Write-Host "Каталог добавлен в PATH пользователя."
    $pathChanged = $true
}
# В текущем окне PATH меняется отдельно: системная переменная доедет до
# него только при следующем запуске оболочки.
if ($env:Path -notlike "*$dest*") { $env:Path = "$env:Path;$dest" }

# ---- 6. Что дальше ----
Write-Host ""
Write-Host "Готово: $target" -ForegroundColor Green
Write-Host "Релей запомнен в $configDir\config"
if ($pathChanged) {
    Write-Host "В новых окнах PowerShell команда ferry будет доступна сразу;"
    Write-Host "в этом окне она уже работает."
}
Write-Host ""
Write-Host "Отправить файл:"
Write-Host "  ferry send 'D:\видео\съёмка.mov'"
Write-Host ""
Write-Host "Раздача живёт, пока запущена команда — закроете окно, она остановится."
