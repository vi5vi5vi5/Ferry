#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Маленький JSON для ядра и CLI.
//
// Зачем свой, а не готовая библиотека: ядро сознательно собирается без
// внешних зависимостей, а единственная альтернатива — втащить в него
// двадцатитысячестрочный заголовок ради пяти полей манифеста. Сервер этим
// кодом не пользуется вовсе: у него есть QJsonDocument, а манифест он и не
// читает — для него это непрозрачные байты.
//
// Что поддерживается: весь JSON по RFC 8259, включая \u-экранирование и
// суррогатные пары, потому что по ту сторону провода стоит JSON.stringify
// из браузера и он такое порождает.
// Чего нет: комментариев, хвостовых запятых и прочих послаблений.
namespace ferry::json {

class Value
{
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    static Value makeNull() { return Value(); }
    static Value make(bool v);
    static Value make(int64_t v);
    static Value make(double v);
    static Value make(const std::string &v);
    static Value make(const char *v) { return make(std::string(v)); }
    static Value array();
    static Value object();

    Type type() const { return m_type; }
    bool isNull() const { return m_type == Type::Null; }
    bool isObject() const { return m_type == Type::Object; }
    bool isArray() const { return m_type == Type::Array; }
    bool isString() const { return m_type == Type::String; }
    bool isNumber() const { return m_type == Type::Number; }
    bool isBool() const { return m_type == Type::Bool; }

    // Геттеры с умолчанием: «поля нет» и «поле не того типа» для нас одно и
    // то же — на проводе и там и там мусор, и разбираться в оттенках незачем.
    bool toBool(bool def = false) const;
    int64_t toInt(int64_t def = 0) const;
    double toDouble(double def = 0.0) const;
    std::string toString(const std::string &def = {}) const;

    // Объект.
    bool has(const std::string &key) const;
    const Value &operator[](const std::string &key) const;
    void set(const std::string &key, Value v);

    // Массив.
    size_t size() const;
    const Value &at(size_t i) const;
    void push(Value v);

    std::string dump() const;

    // Разбор. err получает человекочитаемое место ошибки.
    static bool parse(const std::string &text, Value &out, std::string *err = nullptr);

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    double m_num = 0.0;
    int64_t m_int = 0;
    bool m_isInt = false;      // число пришло целым — печатаем без точки
    std::string m_str;
    std::vector<Value> m_arr;
    std::vector<std::pair<std::string, Value>> m_obj;
};

// Экранирование строки по правилам JSON, с кавычками по краям. Вынесено
// наружу: пригождается, когда JSON собирается руками в горячем месте.
std::string escape(const std::string &s);

} // namespace ferry::json
