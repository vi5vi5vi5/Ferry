#include "config/ServerConfig.h"

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QTextStream>

#include <algorithm>

namespace {

// Ключи хранятся как "секция.ключ" в нижнем регистре: так одинаково
// ложатся и строки файла, и переменные окружения FERRY_TRANSFER_WINDOW_MB.
using Values = QHash<QString, QString>;

bool readIniFile(const QString &path, Values &out, QStringList *warnings)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QTextStream in(&f);
    QString section;
    int lineNo = 0;
    while (!in.atEnd()) {
        ++lineNo;
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';')))
            continue;

        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            section = line.mid(1, line.size() - 2).trimmed().toLower();
            continue;
        }

        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0) {
            if (warnings)
                warnings->append(QStringLiteral("%1, строка %2: не похоже на «ключ = значение», пропущено")
                                     .arg(path).arg(lineNo));
            continue;
        }
        const QString key = line.left(eq).trimmed().toLower();
        QString value = line.mid(eq + 1).trimmed();
        // Кавычки по краям снимаем: значение может кончаться пробелом,
        // и единственный способ это записать — закавычить.
        if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
            value = value.mid(1, value.size() - 2);
        out.insert(section.isEmpty() ? key : section + QLatin1Char('.') + key, value);
    }
    return true;
}

// FERRY_TRANSFER_WINDOW_MB → transfer.window_mb
void readEnvironment(Values &out)
{
    static const char *kKnown[] = {
        "server.name", "server.public_url",
        "transfer.window_mb", "transfer.ram_budget_mb", "transfer.max_transfers",
        "transfer.max_concurrent", "transfer.default_ttl_s", "transfer.max_ttl_s",
        "limits.create_per_ip_per_min", "limits.max_transfers_per_ip",
        "web.enabled",
        "log.level",
    };
    for (const char *key : kKnown) {
        QString envName = QStringLiteral("FERRY_") + QString::fromLatin1(key).toUpper();
        envName.replace(QLatin1Char('.'), QLatin1Char('_'));
        const QByteArray raw = qgetenv(envName.toLatin1().constData());
        if (!raw.isEmpty())
            out.insert(QString::fromLatin1(key), QString::fromUtf8(raw).trimmed());
    }
}

int intValue(const Values &v, const char *key, int def, int lo, int hi, QStringList *warnings)
{
    const auto it = v.constFind(QString::fromLatin1(key));
    if (it == v.constEnd())
        return def;
    bool ok = false;
    const int parsed = it.value().toInt(&ok);
    if (!ok) {
        if (warnings)
            warnings->append(QStringLiteral("настройка %1: «%2» — не число, взято умолчание %3")
                                 .arg(QString::fromLatin1(key), it.value()).arg(def));
        return def;
    }
    const int clamped = std::clamp(parsed, lo, hi);
    if (clamped != parsed && warnings) {
        warnings->append(QStringLiteral("настройка %1: %2 вне допустимого [%3, %4], взято %5")
                             .arg(QString::fromLatin1(key)).arg(parsed).arg(lo).arg(hi).arg(clamped));
    }
    return clamped;
}

bool boolValue(const Values &v, const char *key, bool def, QStringList *warnings)
{
    const auto it = v.constFind(QString::fromLatin1(key));
    if (it == v.constEnd())
        return def;
    const QString s = it.value().toLower();
    if (s == QLatin1String("true") || s == QLatin1String("1") || s == QLatin1String("yes"))
        return true;
    if (s == QLatin1String("false") || s == QLatin1String("0") || s == QLatin1String("no"))
        return false;
    if (warnings)
        warnings->append(QStringLiteral("настройка %1: «%2» — не да и не нет, взято умолчание")
                             .arg(QString::fromLatin1(key), it.value()));
    return def;
}

QString stringValue(const Values &v, const char *key, const QString &def)
{
    return v.value(QString::fromLatin1(key), def);
}

} // namespace

ServerConfig ServerConfig::load(const QString &filePath, QStringList *warnings)
{
    Values values;
    if (!filePath.isEmpty() && !readIniFile(filePath, values, warnings) && warnings) {
        // Отсутствие файла — норма, а не беда: всё имеет умолчание.
        warnings->append(QStringLiteral("конфиг %1 не прочитан — работаем на умолчаниях").arg(filePath));
    }
    readEnvironment(values);

    ServerConfig c;
    c.name = stringValue(values, "server.name", c.name);
    c.publicUrl = stringValue(values, "server.public_url", c.publicUrl);
    // Хвостовой слэш ломал бы склейку ссылки: получилось бы https://host//t/id.
    while (c.publicUrl.endsWith(QLatin1Char('/')))
        c.publicUrl.chop(1);

    c.windowMb = intValue(values, "transfer.window_mb", c.windowMb, 1, 4096, warnings);
    c.ramBudgetMb = intValue(values, "transfer.ram_budget_mb", c.ramBudgetMb, 16, 262144, warnings);
    c.maxTransfers = intValue(values, "transfer.max_transfers", c.maxTransfers, 1, 10000, warnings);
    c.maxConcurrent = intValue(values, "transfer.max_concurrent", c.maxConcurrent, 1, 64, warnings);
    c.defaultTtlS = intValue(values, "transfer.default_ttl_s", c.defaultTtlS, 60, 30 * 24 * 3600, warnings);
    c.maxTtlS = intValue(values, "transfer.max_ttl_s", c.maxTtlS, 60, 30 * 24 * 3600, warnings);
    if (c.defaultTtlS > c.maxTtlS) {
        if (warnings)
            warnings->append(QStringLiteral("transfer.default_ttl_s больше max_ttl_s — обрезано по максимуму"));
        c.defaultTtlS = c.maxTtlS;
    }

    c.createPerIpPerMin = intValue(values, "limits.create_per_ip_per_min", c.createPerIpPerMin, 1, 10000, warnings);
    c.maxTransfersPerIp = intValue(values, "limits.max_transfers_per_ip", c.maxTransfersPerIp, 1, 10000, warnings);

    c.webEnabled = boolValue(values, "web.enabled", c.webEnabled, warnings);

    const auto lvl = values.constFind(QStringLiteral("log.level"));
    if (lvl != values.constEnd()) {
        bool ok = false;
        c.logLevel = Log::levelFromString(lvl.value(), &ok);
        if (!ok && warnings)
            warnings->append(QStringLiteral("log.level: «%1» не опознан — взято errors").arg(lvl.value()));
    }

    // Окно больше глобального бюджета — это не настройка, а опечатка:
    // первая же раздача упёрлась бы в потолок целиком.
    if (c.windowBytes() > c.ramBudgetBytes()) {
        if (warnings)
            warnings->append(QStringLiteral("transfer.window_mb больше ram_budget_mb — окно обрезано по бюджету"));
        c.windowMb = std::max(1, c.ramBudgetMb);
    }

    return c;
}
