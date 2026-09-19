// ============================================================
//  Ferry — релей больших файлов и папок.
//
//  Сервер ничего не хранит на диске. В оперативке — идентификатор раздачи,
//  сокеты и скользящее окно байтов, которое тут же вытесняется. Перезапуск
//  сервера = все раздачи умерли, и это нормально (§1 проектного документа).
//
//  Две точки входа:
//    HTTP      /api/*, страница раздачи /t/<id>, установщик клиента;
//    WebSocket /wsf — управление и сами чанки.
//  Снаружи оба порта закрыты: наружу их публикует nginx по https и wss
//  (см. docker-compose.yml и proxy/).
// ============================================================
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <QTimer>

#include <cstdio>

#include "config/Log.h"
#include "config/ServerConfig.h"
#include "core/ChallengeStore.h"
#include "core/Protocol.h"
#include "core/TransferRegistry.h"
#include "network/HttpApi.h"
#include "network/HttpFileServer.h"
#include "network/TransferServer.h"

#ifndef FERRY_COMMIT
#define FERRY_COMMIT "unknown"
#endif
#ifndef FERRY_MODIFIED
#define FERRY_MODIFIED 0
#endif
#ifndef FERRY_BUILD_TIME
#define FERRY_BUILD_TIME "unknown"
#endif
#ifndef FERRY_WEB_ROOT_DEFAULT
#define FERRY_WEB_ROOT_DEFAULT "web"
#endif

namespace {

void printUsage()
{
    std::printf(
        "Ferry — релей больших файлов.\n"
        "\n"
        "  --http-port <порт>    HTTP: API, страницы, установщик (по умолчанию 8080)\n"
        "  --ws-port <порт>      WebSocket-релей (по умолчанию 9000)\n"
        "  --web-root <путь>     где лежат страницы и /dl (по умолчанию " FERRY_WEB_ROOT_DEFAULT ")\n"
        "  --config-dir <путь>   где искать ferry.conf (по умолчанию рядом с web-root)\n"
        "  --version             версия и коммит сборки\n"
        "  --help                это сообщение\n"
        "\n"
        "Настройки читаются из <config-dir>/ferry.conf и переменных окружения\n"
        "FERRY_*. Сервер этот файл только читает: на диск он не пишет ничего.\n");
}

QString argValue(const QStringList &args, const QString &name, const QString &def)
{
    const int i = args.indexOf(name);
    if (i >= 0 && i + 1 < args.size())
        return args.at(i + 1);
    return def;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList args = QCoreApplication::arguments();

    if (args.contains(QStringLiteral("--help")) || args.contains(QStringLiteral("-h"))) {
        printUsage();
        return 0;
    }
    if (args.contains(QStringLiteral("--version"))) {
        std::printf("Ferry server, протокол v%d, сборка %s%s (%s)\n", int(ferry::kProtocolVersion),
                    FERRY_COMMIT, FERRY_MODIFIED ? " с локальными изменениями" : "",
                    FERRY_BUILD_TIME);
        return 0;
    }

    const quint16 httpPort = quint16(argValue(args, QStringLiteral("--http-port"),
                                              QStringLiteral("8080")).toUShort());
    const quint16 wsPort = quint16(argValue(args, QStringLiteral("--ws-port"),
                                            QStringLiteral("9000")).toUShort());
    const QString webRoot = argValue(args, QStringLiteral("--web-root"),
                                     QStringLiteral(FERRY_WEB_ROOT_DEFAULT));
    const QString configDir = argValue(args, QStringLiteral("--config-dir"),
                                       QFileInfo(webRoot).absolutePath());
    const QString configPath = QDir(configDir).filePath(QStringLiteral("ferry.conf"));

    // Конфиг читается ДО установки журнала: уровень журнала задаётся в нём
    // же. Всё, о чём стоит сказать по дороге, копится в warnings и
    // печатается сразу после — иначе жалобы на опечатки уходили бы в
    // никуда именно тогда, когда они нужнее всего.
    QStringList warnings;
    const ServerConfig config = ServerConfig::load(configPath, &warnings);
    Log::install(config.logLevel);

    qCInfo(lcApp).noquote()
        << QStringLiteral("%1 — Ferry, протокол v%2, сборка %3%4")
               .arg(config.name)
               .arg(int(ferry::kProtocolVersion))
               .arg(QStringLiteral(FERRY_COMMIT))
               .arg(FERRY_MODIFIED ? QStringLiteral(" (с локальными изменениями)") : QString());
    for (const QString &w : warnings)
        qWarning().noquote() << w;

    qCInfo(lcApp).noquote()
        << QStringLiteral("окно на раздачу %1 МиБ, бюджет памяти %2 МиБ, раздач не больше %3, "
                          "получателей на раздачу не больше %4")
               .arg(config.windowMb).arg(config.ramBudgetMb)
               .arg(config.maxTransfers).arg(config.maxConcurrent);
    qCInfo(lcApp).noquote()
        << QStringLiteral("журнал: %1%2")
               .arg(Log::levelToString(config.logLevel))
               .arg(Log::keepsIdentifiers()
                        ? QStringLiteral(" — идентификаторы раздач ПОПАДАЮТ в вывод")
                        : QStringLiteral(" — идентификаторов раздач в выводе нет"));

    auto *registry = new TransferRegistry(config, &app);
    auto *challenges = new ChallengeStore;
    auto *api = new HttpApi(registry, challenges, config, &app);
    // Вебсокет создаётся первым: HTTP-транспорт отдаёт ему соединения,
    // пришедшие с Upgrade, и наружу у релея остаётся один порт.
    auto *ws = new TransferServer(wsPort, registry, challenges, config, &app);
    auto *http = new HttpFileServer(httpPort, webRoot, api, ws, config.webEnabled,
                                    config.publicUrl, &app);

    if (!http->isListening() || !ws->isListening()) {
        qCritical().noquote() << QStringLiteral("не удалось занять порты — выходим");
        delete challenges;
        return 1;
    }

    QObject::connect(&app, &QCoreApplication::aboutToQuit, [challenges] { delete challenges; });

    qCInfo(lcApp).noquote() << QStringLiteral("готов");
    return app.exec();
}
