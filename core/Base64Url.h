#pragma once

#include <cstdint>
#include <string>
#include <vector>

// base64url без выравнивания (RFC 4648 §5): алфавит с '-' и '_', хвостовых
// '=' нет. Именно в таком виде живут идентификатор раздачи и ключ в ссылке —
// обычный base64 туда не годится, потому что '+' и '/' в URL значат другое,
// а '=' пришлось бы процентно кодировать и ссылка перестала бы читаться
// глазами.
namespace ferry {

std::string base64UrlEncode(const uint8_t *data, size_t len);
std::string base64UrlEncode(const std::vector<uint8_t> &data);

// Принимает и вариант с '=' на конце, и классический алфавит с '+' и '/':
// ссылку могли пропустить через чужой инструмент, который «починил» её на
// свой лад. Возвращает false, если строка содержит что-то ещё или её длина
// невозможна для base64.
bool base64UrlDecode(const std::string &text, std::vector<uint8_t> &out);

} // namespace ferry
