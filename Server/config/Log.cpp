#include "config/Log.h"

#include <QByteArray>

#include <cstdio>

Q_LOGGING_CATEGORY(lcApp, "ferry.app")

namespace {

// Уровень живёт здесь, а не в ServerConfig: обработчик сообщений Qt —
// свободная функция, и тащить в неё указатель на конфиг не через что.
// Меняется ровно один раз, в Log::install из main.
Log::Level g_level = Log::Level::Errors;

// Правило одно и умещается в строку: «что-то не так» печатаем всегда,
// «что-то произошло» — только когда журнал разрешён.
void handler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    if (g_level == Log::Level::Off)
        return;

    const bool problem = (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg);
    const bool startup = ctx.category && qstrcmp(ctx.category, lcApp().categoryName()) == 0;
    if (!problem && !startup && g_level != Log::Level::Normal)
        return;

    const char *prefix = "";
    if (type == QtWarningMsg)
        prefix = "WARN: ";
    else if (type == QtCriticalMsg || type == QtFatalMsg)
        prefix = "ERROR: ";

    // Всё в stdout, включая ошибки. Разделение на два потока в MeetUp
    // проверялось и было откачено: `docker logs` собирает stdout и stderr по
    // отдельности и показывает вперемешку, поэтому жалоба на опечатку в
    // конфиге выезжала выше строки старта и читалась как остаток прошлого
    // запуска. Кто есть кто, видно по префиксу.
    const QByteArray line = msg.toUtf8();
    std::fprintf(stdout, "%s%s\n", prefix, line.constData());
    std::fflush(stdout);
}

} // namespace

namespace Log {

Level levelFromString(const QString &s, bool *ok)
{
    if (ok)
        *ok = true;
    const QString v = s.trimmed().toLower();
    if (v == QLatin1String("off"))
        return Level::Off;
    if (v == QLatin1String("errors"))
        return Level::Errors;
    if (v == QLatin1String("normal"))
        return Level::Normal;
    if (ok)
        *ok = false;
    return Level::Errors;
}

QString levelToString(Level level)
{
    switch (level) {
    case Level::Off:
        return QStringLiteral("off");
    case Level::Normal:
        return QStringLiteral("normal");
    case Level::Errors:
        break;
    }
    return QStringLiteral("errors");
}

void install(Level level)
{
    g_level = level;
    qInstallMessageHandler(handler);
}

bool keepsIdentifiers()
{
    return g_level == Level::Normal;
}

} // namespace Log
