#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Cli/platform/Platform.h"
#include "core/VolumeLayout.h"

// Том как одно байтовое пространство — поверх одного файла или дерева.
//
// Зачем это отдельным типом. Сервер видит линейный поток чанков и не
// знает, один там файл или сорок тысяч (§3). Ровно то же должно быть
// верно и для остального клиента: отправитель читает «байты тома по
// смещению», получатель пишет «байты тома по смещению», и ни тот ни
// другой не должны знать про границы файлов. Вся разница — здесь.
//
// Читаем и пишем ПО СМЕЩЕНИЮ, без общего курсора: чанки приходят
// вразнобой (две волны, докачка), и seek+read означал бы гонку. На
// границе файлов один чанк раскладывается на несколько кусков — это и
// делает slice() из ядра.
namespace ferry::cli {

class VolumeFile
{
public:
    ~VolumeFile();
    VolumeFile() = default;
    VolumeFile(const VolumeFile &) = delete;
    VolumeFile &operator=(const VolumeFile &) = delete;

    // Один файл во весь том.
    bool openSingle(const std::string &path, bool forWrite, uint64_t size);

    // Дерево: пути из раскладки — относительные, от root.
    //
    // На запись создаются и каталоги, и сами файлы, но ЛЕНИВО, при первом
    // обращении. Сорок тысяч файлов, созданных заранее, — это сорок тысяч
    // системных вызовов до первого принятого байта; человек в это время
    // смотрит на замерший ноль процентов и думает, что всё сломалось.
    bool openTree(const std::string &root, const VolumeLayout &layout, bool forWrite);

    // Пустые файлы и пустые директории. В том они байт не вносят, а на
    // диске быть обязаны — иначе дерево приедет неполным.
    bool createEmpties(const std::string &root, const Manifest &manifest);

    int64_t readAt(void *buf, size_t len, uint64_t offset);
    int64_t writeAt(const void *buf, size_t len, uint64_t offset);

    bool sync();
    void close();

    const std::string &error() const { return m_error; }

private:
    // Дескриптор файла дерева, открываемый по требованию.
    //
    // Кэш маленький и простой — кольцо на восемь штук. Обе волны читают
    // и пишут почти подряд, так что попадание почти всегда в первый же
    // элемент, а ограничение сверху нужно, чтобы не упереться в лимит
    // открытых дескрипторов на томе из сорока тысяч файлов.
    struct Slot
    {
        size_t file = SIZE_MAX;
        platform::File fd = platform::kInvalidFile;
    };

    platform::File handleFor(size_t file);
    bool ensurePrepared(size_t file);

    bool m_tree = false;
    bool m_write = false;
    std::string m_root;
    const VolumeLayout *m_layout = nullptr;

    // Одиночный файл.
    platform::File m_single = platform::kInvalidFile;

    // Дерево.
    std::vector<Slot> m_slots;
    size_t m_nextSlot = 0;
    std::vector<bool> m_prepared;   // файл уже создан и растянут

    std::string m_error;
};

} // namespace ferry::cli
