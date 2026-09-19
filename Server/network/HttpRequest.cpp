#include "network/HttpRequest.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>

QByteArray HttpRequest::cookie(const QByteArray &name) const
{
    return cookieValue(header(QByteArrayLiteral("cookie")), name);
}

QByteArray HttpRequest::cookieValue(const QByteArray &cookieHeader, const QByteArray &name)
{
    const QList<QByteArray> parts = cookieHeader.split(';');
    for (const QByteArray &part : parts) {
        const QByteArray p = part.trimmed();
        const int eq = p.indexOf('=');
        if (eq > 0 && p.left(eq) == name)
            return p.mid(eq + 1);
    }
    return {};
}

QJsonObject HttpRequest::jsonBody(bool *ok) const
{
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (ok)
        *ok = doc.isObject();
    return doc.object();
}

HttpRequestParser::State HttpRequestParser::fail(int status)
{
    m_errorStatus = status;
    m_state = State::Error;
    return m_state;
}

HttpRequestParser::State HttpRequestParser::feed(const QByteArray &chunk)
{
    if (m_state != State::NeedMore)
        return m_state;

    m_buf += chunk;

    if (m_bodyNeed < 0) {
        const int headEnd = m_buf.indexOf(QByteArrayLiteral("\r\n\r\n"));
        if (headEnd < 0)
            return m_buf.size() > kMaxHeaderBytes ? fail(431) : m_state;
        if (parseHead(headEnd) == State::Error)
            return m_state;
    }

    if (m_buf.size() < m_bodyNeed)
        return m_state;

    // Лишний хвост (следующий pipelined-запрос) игнорируем: после ответа
    // соединение закрывается (Connection: close).
    m_req.body = m_buf.left(int(m_bodyNeed));
    m_buf.clear();
    m_state = State::Done;
    return m_state;
}

HttpRequestParser::State HttpRequestParser::parseHead(int headEnd)
{
    const QByteArray head = m_buf.left(headEnd);
    m_buf.remove(0, headEnd + 4);

    const QList<QByteArray> lines = head.split('\n');
    if (lines.isEmpty())
        return fail(400);

    // Стартовая строка: "METHOD /path[?query] HTTP/1.1".
    const QList<QByteArray> parts = lines.first().trimmed().split(' ');
    if (parts.size() < 2 || parts.first().isEmpty())
        return fail(400);
    m_req.method = parts.at(0);

    QByteArray target = parts.at(1);
    const int query = target.indexOf('?');
    if (query >= 0)
        target = target.left(query);
    m_req.path = QUrl::fromPercentEncoding(target);
    if (m_req.path.isEmpty() || !m_req.path.startsWith(QLatin1Char('/')))
        return fail(400);

    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        if (line.isEmpty())
            continue;
        const int colon = line.indexOf(':');
        if (colon <= 0)
            return fail(400);
        m_req.headers.insert(line.left(colon).toLower(), line.mid(colon + 1).trimmed());
    }

    // Chunked не поддерживаем: наши клиенты (fetch с готовым телом, curl,
    // свой мини-клиент в Cli/net) всегда шлют Content-Length.
    if (m_req.headers.contains(QByteArrayLiteral("transfer-encoding")))
        return fail(501);

    m_bodyNeed = 0;
    const QByteArray lenRaw = m_req.header(QByteArrayLiteral("content-length"));
    if (!lenRaw.isEmpty()) {
        bool okNum = false;
        m_bodyNeed = lenRaw.toLongLong(&okNum);
        if (!okNum || m_bodyNeed < 0)
            return fail(400);
        if (m_bodyNeed > kMaxBodyBytes)
            return fail(413);
    }
    return m_state;
}
