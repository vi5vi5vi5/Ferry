#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>

class QJsonObject;

// Разобранный HTTP-запрос: метод, путь, заголовки, тело.
// Ровно то, что нужно API и статике; ничего лишнего (query пока не разбираем).
struct HttpRequest
{
    QByteArray method;                       // "GET", "POST", ...
    QString path;                            // декодированный путь без query: "/api/me"
    QHash<QByteArray, QByteArray> headers;   // имена приведены к нижнему регистру
    QByteArray body;

    QByteArray header(const QByteArray &nameLower) const { return headers.value(nameLower); }

    // Значение куки по имени из заголовка "Cookie: a=1; b=2".
    QByteArray cookie(const QByteArray &name) const;

    // То же для «чужого» заголовка Cookie — например, из WS-хендшейка.
    static QByteArray cookieValue(const QByteArray &cookieHeader, const QByteArray &name);

    // Тело как JSON-объект; *ok = false, если тело не валидный JSON-объект.
    QJsonObject jsonBody(bool *ok = nullptr) const;
};

// Инкрементальный сборщик запроса: сокет может отдавать данные кусками,
// feed() копит их, пока не соберётся полный запрос (заголовки + тело
// по Content-Length). Один парсер обслуживает одно соединение.
class HttpRequestParser
{
public:
    enum class State {
        NeedMore,   // данных пока не хватает
        Done,       // запрос собран, можно обрабатывать
        Error,      // запрос сломан или превысил лимиты — ответить errorStatus()
    };

    State feed(const QByteArray &chunk);

    const HttpRequest &request() const { return m_req; }
    int errorStatus() const { return m_errorStatus; }

    // Лимиты: заголовки — от бесконечного мусора, тело — от исчерпания
    // памяти. У Ferry по HTTP вообще нечего присылать: том едет по
    // WebSocket, а POST /api/transfers — это пустое тело. 64 КиБ с
    // огромным запасом.
    static constexpr qint64 kMaxHeaderBytes = 16 * 1024;
    static constexpr qint64 kMaxBodyBytes   = 64 * 1024;

private:
    State fail(int status);
    State parseHead(int headEnd);

    QByteArray m_buf;
    HttpRequest m_req;
    qint64 m_bodyNeed = -1;     // -1 — заголовки ещё не разобраны
    int m_errorStatus = 0;
    State m_state = State::NeedMore;
};
