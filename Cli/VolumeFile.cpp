#include "Cli/VolumeFile.h"

#include <algorithm>

namespace ferry::cli {
namespace {

constexpr size_t kOpenSlots = 8;

// Родительский каталог пути «a/b/c» — это «a/b». Пустая строка, если
// родителя нет.
std::string parentOf(const std::string &path)
{
    const size_t cut = path.rfind('/');
    return cut == std::string::npos ? std::string() : path.substr(0, cut);
}

} // namespace

VolumeFile::~VolumeFile()
{
    close();
}

bool VolumeFile::openSingle(const std::string &path, bool forWrite, uint64_t size)
{
    close();
    m_tree = false;
    m_write = forWrite;

    m_single = forWrite ? platform::fileOpenReadWrite(path) : platform::fileOpenRead(path);
    if (m_single == platform::kInvalidFile) {
        m_error = "не удалось открыть " + path + ": " + platform::lastFileError();
        return false;
    }
    // Растягиваем сразу на полный размер: дальше пишем по смещениям, и
    // место должно быть заранее — иначе первая же дырка превратится в
    // ошибку записи на середине тома.
    if (forWrite && !platform::fileTruncate(m_single, size)) {
        m_error = "не хватает места под " + path + ": " + platform::lastFileError();
        platform::fileClose(m_single);
        m_single = platform::kInvalidFile;
        return false;
    }
    return true;
}

bool VolumeFile::openTree(const std::string &root, const VolumeLayout &layout, bool forWrite)
{
    close();
    m_tree = true;
    m_write = forWrite;
    m_root = root;
    m_layout = &layout;
    m_slots.assign(kOpenSlots, Slot{});
    m_prepared.assign(layout.files().size(), false);
    return true;
}

bool VolumeFile::createEmpties(const std::string &root, const Manifest &manifest)
{
    if (!manifest.isTree())
        return true;

    for (const ManifestEntry &e : manifest.entries) {
        const std::string full = root + "/" + e.path;
        if (e.isDir) {
            if (!platform::makeDirectories(full)) {
                m_error = "не удалось создать каталог " + e.path;
                return false;
            }
            continue;
        }
        if (e.size != 0)
            continue;   // непустые файлы создаются лениво, при первой записи

        const std::string parent = parentOf(full);
        if (!parent.empty() && !platform::makeDirectories(parent)) {
            m_error = "не удалось создать каталог для " + e.path;
            return false;
        }
        const platform::File fd = platform::fileOpenReadWrite(full);
        if (fd == platform::kInvalidFile) {
            m_error = "не удалось создать " + e.path;
            return false;
        }
        platform::fileClose(fd);
    }
    return true;
}

bool VolumeFile::ensurePrepared(size_t file)
{
    if (m_prepared[file])
        return true;

    const VolumeLayout::FileSpan &f = m_layout->files()[file];
    const std::string full = m_root + "/" + f.path;

    if (m_write) {
        const std::string parent = parentOf(full);
        if (!parent.empty() && !platform::makeDirectories(parent)) {
            m_error = "не удалось создать каталог для " + f.path;
            return false;
        }
    }
    m_prepared[file] = true;
    return true;
}

platform::File VolumeFile::handleFor(size_t file)
{
    for (const Slot &s : m_slots) {
        if (s.file == file)
            return s.fd;
    }
    if (!ensurePrepared(file))
        return platform::kInvalidFile;

    const VolumeLayout::FileSpan &f = m_layout->files()[file];
    const std::string full = m_root + "/" + f.path;

    const platform::File fd =
        m_write ? platform::fileOpenReadWrite(full) : platform::fileOpenRead(full);
    if (fd == platform::kInvalidFile) {
        m_error = "не удалось открыть " + f.path + ": " + platform::lastFileError();
        return platform::kInvalidFile;
    }
    if (m_write && !platform::fileTruncate(fd, f.size)) {
        m_error = "не хватает места под " + f.path + ": " + platform::lastFileError();
        platform::fileClose(fd);
        return platform::kInvalidFile;
    }

    // Вытесняем по кругу. Слот мог быть занят — закрываем то, что в нём.
    Slot &slot = m_slots[m_nextSlot];
    m_nextSlot = (m_nextSlot + 1) % m_slots.size();
    if (slot.fd != platform::kInvalidFile)
        platform::fileClose(slot.fd);
    slot.file = file;
    slot.fd = fd;
    return fd;
}

int64_t VolumeFile::readAt(void *buf, size_t len, uint64_t offset)
{
    if (!m_tree) {
        const int64_t got = platform::fileReadAt(m_single, buf, len, offset);
        if (got < 0)
            m_error = "не удалось прочитать файл: " + platform::lastFileError();
        else if (got != int64_t(len))
            m_error = "файл стал короче, чем был в начале раздачи, — его изменили во время раздачи";
        return got;
    }

    auto *out = static_cast<uint8_t *>(buf);
    int64_t done = 0;
    for (const VolumeLayout::Piece &p : m_layout->slice(offset, len)) {
        const platform::File fd = handleFor(p.file);
        if (fd == platform::kInvalidFile)
            return -1;
        const int64_t got = platform::fileReadAt(fd, out + done, size_t(p.length), p.offset);
        if (got < 0) {
            m_error = "не удалось прочитать " + m_layout->files()[p.file].path + ": "
                      + platform::lastFileError();
            return -1;
        }
        if (got != int64_t(p.length)) {
            m_error = m_layout->files()[p.file].path
                      + " стал короче, чем был при описи, — его изменили во время раздачи";
            return -1;
        }
        done += got;
    }
    return done;
}

int64_t VolumeFile::writeAt(const void *buf, size_t len, uint64_t offset)
{
    if (!m_tree) {
        const int64_t put = platform::fileWriteAt(m_single, buf, len, offset);
        if (put != int64_t(len))
            m_error = "не удалось записать файл: " + platform::lastFileError();
        return put;
    }

    const auto *in = static_cast<const uint8_t *>(buf);
    int64_t done = 0;
    for (const VolumeLayout::Piece &p : m_layout->slice(offset, len)) {
        const platform::File fd = handleFor(p.file);
        if (fd == platform::kInvalidFile)
            return -1;
        const int64_t put = platform::fileWriteAt(fd, in + done, size_t(p.length), p.offset);
        if (put != int64_t(p.length)) {
            m_error = "не удалось записать " + m_layout->files()[p.file].path + ": "
                      + platform::lastFileError();
            return -1;
        }
        done += put;
    }
    return done;
}

bool VolumeFile::sync()
{
    if (!m_tree)
        return m_single == platform::kInvalidFile || platform::fileSync(m_single);

    // Синхронизируем только то, что сейчас открыто. Остальное уже закрыто,
    // а закрытие дескриптора сбрасывает буферы на диск.
    bool ok = true;
    for (const Slot &s : m_slots) {
        if (s.fd != platform::kInvalidFile)
            ok = platform::fileSync(s.fd) && ok;
    }
    return ok;
}

void VolumeFile::close()
{
    if (m_single != platform::kInvalidFile) {
        platform::fileClose(m_single);
        m_single = platform::kInvalidFile;
    }
    for (Slot &s : m_slots) {
        if (s.fd != platform::kInvalidFile)
            platform::fileClose(s.fd);
        s = Slot{};
    }
    m_slots.clear();
    m_prepared.clear();
    m_nextSlot = 0;
    m_layout = nullptr;
    m_tree = false;
}

} // namespace ferry::cli
