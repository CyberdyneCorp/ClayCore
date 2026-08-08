#include "file_bytes.h"

#include <cstdio>

namespace clay {
namespace io {
namespace detail {

IoStatus read_whole_file(const std::string& path, std::vector<std::uint8_t>* bytes,
                         std::size_t max_bytes) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return IoStatus::fail(IoError::FileNotFound, path);

    // Every step is checked: a directory opens successfully on glibc and only
    // gives itself away at the tell (LONG_MAX) or the read (EISDIR), and which
    // of the two it is differs by platform.
    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return IoStatus::fail(IoError::ReadFailed, path);
    }
    const long size = std::ftell(f);
    if (size < 0) {
        std::fclose(f);
        return IoStatus::fail(IoError::ReadFailed, path);
    }
    if (static_cast<unsigned long long>(size) > max_bytes) {
        std::fclose(f);
        return IoStatus::fail(IoError::BudgetExceeded, path);
    }
    if (std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return IoStatus::fail(IoError::ReadFailed, path);
    }

    bytes->resize(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(bytes->data(), 1, bytes->size(), f);
    std::fclose(f);
    if (read != bytes->size()) {
        bytes->clear();
        return IoStatus::fail(IoError::ReadFailed, path);
    }
    return IoStatus::success();
}

IoStatus write_whole_file(const std::string& path, const void* data, std::size_t size) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return IoStatus::fail(IoError::WriteFailed, path);
    const std::size_t written = size ? std::fwrite(data, 1, size, f) : 0;
    // A write can fail at the flush rather than the fwrite, so the close is
    // part of the result and not an afterthought.
    const bool closed = std::fclose(f) == 0;
    return written == size && closed ? IoStatus::success()
                                     : IoStatus::fail(IoError::WriteFailed, path);
}

}  // namespace detail
}  // namespace io
}  // namespace clay
