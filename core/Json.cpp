#include "Json.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ferry::json {
namespace {

const Value &nullValue()
{
    static const Value v;
    return v;
}

// UTF-8 для одной кодовой точки. Суррогатные пары склеивает вызывающий.
void appendUtf8(std::string &out, uint32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

class Parser
{
public:
    explicit Parser(const std::string &text) : m_s(text) {}

    bool run(Value &out)
    {
        skipWs();
        if (!parseValue(out))
            return false;
        skipWs();
        if (m_i != m_s.size())
            return fail("лишние данные после значения");
        return true;
    }

    const std::string &error() const { return m_err; }

private:
    bool fail(const char *what)
    {
        if (m_err.empty()) {
            char buf[192];
            std::snprintf(buf, sizeof(buf), "%s (позиция %zu)", what, m_i);
            m_err = buf;
        }
        return false;
    }

    void skipWs()
    {
        while (m_i < m_s.size()) {
            const char c = m_s[m_i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++m_i;
            else
                break;
        }
    }

    bool literal(const char *word)
    {
        const size_t n = std::char_traits<char>::length(word);
        if (m_s.compare(m_i, n, word) != 0)
            return false;
        m_i += n;
        return true;
    }

    bool parseValue(Value &out)
    {
        if (m_depth > kMaxDepth)
            return fail("слишком глубокая вложенность");
        if (m_i >= m_s.size())
            return fail("значение оборвалось");

        switch (m_s[m_i]) {
        case '{':
            return parseObject(out);
        case '[':
            return parseArray(out);
        case '"': {
            std::string s;
            if (!parseString(s))
                return false;
            out = Value::make(s);
            return true;
        }
        case 't':
            if (!literal("true"))
                return fail("неопознанный литерал");
            out = Value::make(true);
            return true;
        case 'f':
            if (!literal("false"))
                return fail("неопознанный литерал");
            out = Value::make(false);
            return true;
        case 'n':
            if (!literal("null"))
                return fail("неопознанный литерал");
            out = Value::makeNull();
            return true;
        default:
            return parseNumber(out);
        }
    }

    bool parseObject(Value &out)
    {
        ++m_i;  // открывающая фигурная скобка
        ++m_depth;
        out = Value::object();
        skipWs();
        if (m_i < m_s.size() && m_s[m_i] == '}') {
            ++m_i;
            --m_depth;
            return true;
        }
        for (;;) {
            skipWs();
            std::string key;
            if (!parseString(key))
                return fail("ожидалось имя поля");
            skipWs();
            if (m_i >= m_s.size() || m_s[m_i] != ':')
                return fail("ожидалось двоеточие");
            ++m_i;
            skipWs();
            Value v;
            if (!parseValue(v))
                return false;
            out.set(key, std::move(v));
            skipWs();
            if (m_i >= m_s.size())
                return fail("объект не закрыт");
            if (m_s[m_i] == ',') {
                ++m_i;
                continue;
            }
            if (m_s[m_i] == '}') {
                ++m_i;
                --m_depth;
                return true;
            }
            return fail("ожидалась запятая или закрывающая скобка");
        }
    }

    bool parseArray(Value &out)
    {
        ++m_i;  // открывающая квадратная скобка
        ++m_depth;
        out = Value::array();
        skipWs();
        if (m_i < m_s.size() && m_s[m_i] == ']') {
            ++m_i;
            --m_depth;
            return true;
        }
        for (;;) {
            skipWs();
            Value v;
            if (!parseValue(v))
                return false;
            out.push(std::move(v));
            skipWs();
            if (m_i >= m_s.size())
                return fail("массив не закрыт");
            if (m_s[m_i] == ',') {
                ++m_i;
                continue;
            }
            if (m_s[m_i] == ']') {
                ++m_i;
                --m_depth;
                return true;
            }
            return fail("ожидалась запятая или закрывающая скобка");
        }
    }

    bool hex4(uint32_t &out)
    {
        if (m_i + 4 > m_s.size())
            return false;
        out = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = m_s[m_i + static_cast<size_t>(k)];
            out <<= 4;
            if (c >= '0' && c <= '9')
                out |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f')
                out |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                out |= static_cast<uint32_t>(c - 'A' + 10);
            else
                return false;
        }
        m_i += 4;
        return true;
    }

    bool parseString(std::string &out)
    {
        if (m_i >= m_s.size() || m_s[m_i] != '"')
            return fail("ожидалась строка");
        ++m_i;
        out.clear();
        while (m_i < m_s.size()) {
            const unsigned char c = static_cast<unsigned char>(m_s[m_i]);
            if (c == '"') {
                ++m_i;
                return true;
            }
            if (c < 0x20)
                return fail("управляющий символ внутри строки");
            if (c != '\\') {
                out += static_cast<char>(c);
                ++m_i;
                continue;
            }

            ++m_i;
            if (m_i >= m_s.size())
                return fail("строка оборвалась на экранировании");
            const char e = m_s[m_i++];
            switch (e) {
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case '/':  out += '/';  break;
            case 'b':  out += '\b'; break;
            case 'f':  out += '\f'; break;
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case 'u': {
                uint32_t cp = 0;
                if (!hex4(cp))
                    return fail("битая последовательность после обратного слэша u");
                // Суррогатная пара: браузерный JSON.stringify так кодирует
                // всё за пределами BMP, включая эмодзи в именах файлов.
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (m_i + 1 < m_s.size() && m_s[m_i] == '\\' && m_s[m_i + 1] == 'u') {
                        m_i += 2;
                        uint32_t lo = 0;
                        if (!hex4(lo))
                            return fail("битая вторая половина суррогатной пары");
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else
                            cp = 0xFFFD;
                    } else {
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    cp = 0xFFFD;  // одинокая младшая половина пары
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                return fail("неизвестное экранирование");
            }
        }
        return fail("строка не закрыта");
    }

    bool parseNumber(Value &out)
    {
        const size_t start = m_i;
        if (m_i < m_s.size() && m_s[m_i] == '-')
            ++m_i;
        if (m_i >= m_s.size() || m_s[m_i] < '0' || m_s[m_i] > '9')
            return fail("ожидалось число");
        while (m_i < m_s.size() && m_s[m_i] >= '0' && m_s[m_i] <= '9')
            ++m_i;

        bool integral = true;
        if (m_i < m_s.size() && m_s[m_i] == '.') {
            integral = false;
            ++m_i;
            if (m_i >= m_s.size() || m_s[m_i] < '0' || m_s[m_i] > '9')
                return fail("после точки нет цифр");
            while (m_i < m_s.size() && m_s[m_i] >= '0' && m_s[m_i] <= '9')
                ++m_i;
        }
        if (m_i < m_s.size() && (m_s[m_i] == 'e' || m_s[m_i] == 'E')) {
            integral = false;
            ++m_i;
            if (m_i < m_s.size() && (m_s[m_i] == '+' || m_s[m_i] == '-'))
                ++m_i;
            if (m_i >= m_s.size() || m_s[m_i] < '0' || m_s[m_i] > '9')
                return fail("в экспоненте нет цифр");
            while (m_i < m_s.size() && m_s[m_i] >= '0' && m_s[m_i] <= '9')
                ++m_i;
        }

        const std::string text = m_s.substr(start, m_i - start);
        if (integral) {
            // strtoll, а не strtod: размер тома доходит до 2^40, и после
            // прохода через double последние байты потерялись бы молча.
            errno = 0;
            char *end = nullptr;
            const long long v = std::strtoll(text.c_str(), &end, 10);
            if (errno == 0 && end && *end == '\0') {
                out = Value::make(static_cast<int64_t>(v));
                return true;
            }
        }
        out = Value::make(std::strtod(text.c_str(), nullptr));
        return true;
    }

    static constexpr int kMaxDepth = 64;

    const std::string &m_s;
    size_t m_i = 0;
    int m_depth = 0;
    std::string m_err;
};

} // namespace

Value Value::make(bool v)
{
    Value x;
    x.m_type = Type::Bool;
    x.m_bool = v;
    return x;
}

Value Value::make(int64_t v)
{
    Value x;
    x.m_type = Type::Number;
    x.m_int = v;
    x.m_num = static_cast<double>(v);
    x.m_isInt = true;
    return x;
}

Value Value::make(double v)
{
    Value x;
    x.m_type = Type::Number;
    x.m_num = v;
    x.m_int = static_cast<int64_t>(v);
    return x;
}

Value Value::make(const std::string &v)
{
    Value x;
    x.m_type = Type::String;
    x.m_str = v;
    return x;
}

Value Value::array()
{
    Value x;
    x.m_type = Type::Array;
    return x;
}

Value Value::object()
{
    Value x;
    x.m_type = Type::Object;
    return x;
}

bool Value::toBool(bool def) const
{
    return m_type == Type::Bool ? m_bool : def;
}

int64_t Value::toInt(int64_t def) const
{
    if (m_type != Type::Number)
        return def;
    return m_isInt ? m_int : static_cast<int64_t>(m_num);
}

double Value::toDouble(double def) const
{
    return m_type == Type::Number ? m_num : def;
}

std::string Value::toString(const std::string &def) const
{
    return m_type == Type::String ? m_str : def;
}

bool Value::has(const std::string &key) const
{
    if (m_type != Type::Object)
        return false;
    for (const auto &kv : m_obj) {
        if (kv.first == key)
            return true;
    }
    return false;
}

const Value &Value::operator[](const std::string &key) const
{
    if (m_type == Type::Object) {
        for (const auto &kv : m_obj) {
            if (kv.first == key)
                return kv.second;
        }
    }
    return nullValue();
}

void Value::set(const std::string &key, Value v)
{
    if (m_type != Type::Object) {
        m_type = Type::Object;
        m_obj.clear();
    }
    for (auto &kv : m_obj) {
        if (kv.first == key) {
            kv.second = std::move(v);
            return;
        }
    }
    m_obj.emplace_back(key, std::move(v));
}

size_t Value::size() const
{
    if (m_type == Type::Array)
        return m_arr.size();
    if (m_type == Type::Object)
        return m_obj.size();
    return 0;
}

const Value &Value::at(size_t i) const
{
    if (m_type == Type::Array && i < m_arr.size())
        return m_arr[i];
    return nullValue();
}

void Value::push(Value v)
{
    if (m_type != Type::Array) {
        m_type = Type::Array;
        m_arr.clear();
    }
    m_arr.push_back(std::move(v));
}

std::string escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                // UTF-8 отдаём как есть: JSON это разрешает, а экранирование
                // раздуло бы кириллические имена файлов вшестеро.
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
    return out;
}

std::string Value::dump() const
{
    switch (m_type) {
    case Type::Null:
        return "null";
    case Type::Bool:
        return m_bool ? "true" : "false";
    case Type::String:
        return escape(m_str);
    case Type::Number: {
        char buf[40];
        if (m_isInt)
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(m_int));
        else if (std::isfinite(m_num))
            std::snprintf(buf, sizeof(buf), "%.17g", m_num);
        else
            std::snprintf(buf, sizeof(buf), "0");  // JSON не знает inf и nan
        return buf;
    }
    case Type::Array: {
        std::string out = "[";
        for (size_t i = 0; i < m_arr.size(); ++i) {
            if (i)
                out += ',';
            out += m_arr[i].dump();
        }
        return out + "]";
    }
    case Type::Object: {
        std::string out = "{";
        for (size_t i = 0; i < m_obj.size(); ++i) {
            if (i)
                out += ',';
            out += escape(m_obj[i].first);
            out += ':';
            out += m_obj[i].second.dump();
        }
        return out + "}";
    }
    }
    return "null";
}

bool Value::parse(const std::string &text, Value &out, std::string *err)
{
    Parser p(text);
    if (p.run(out))
        return true;
    if (err)
        *err = p.error();
    out = Value();
    return false;
}

} // namespace ferry::json
