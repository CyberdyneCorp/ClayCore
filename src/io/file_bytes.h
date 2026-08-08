#pragma once

// One implementation of "read a whole file" and "write a whole file", shared by
// every loader and writer in this module.
//
// There used to be four copies of the read block and five of the write block,
// one per format, and they had drifted: only the clayspace copy checked ftell
// at all, and none checked fseek. The guard that did exist was also the wrong
// one — glibc's fopen("rb") succeeds on a DIRECTORY, and fseek(SEEK_END)+ftell
// then reports LONG_MAX rather than a negative value, so sizing a buffer from
// it aborted the process (the library builds -fno-exceptions, so the resulting
// std::bad_alloc reaches std::terminate).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "clay/io/result.h"

namespace clay {
namespace io {
namespace detail {

// Reads `path` whole. Fails with ReadFailed on a seek/tell/read error or a
// non-regular file, and with BudgetExceeded when the file is larger than
// `max_bytes` — checked BEFORE the buffer is sized, which is what keeps a
// bogus length out of the allocator.
IoStatus read_whole_file(const std::string& path, std::vector<std::uint8_t>* bytes,
                         std::size_t max_bytes = ImportBudget{}.max_file_bytes);

IoStatus write_whole_file(const std::string& path, const void* data, std::size_t size);

inline IoStatus write_whole_file(const std::string& path,
                                 const std::vector<std::uint8_t>& bytes) {
    return write_whole_file(path, bytes.data(), bytes.size());
}

inline IoStatus write_whole_file(const std::string& path, const std::string& text) {
    return write_whole_file(path, text.data(), text.size());
}

}  // namespace detail
}  // namespace io
}  // namespace clay
