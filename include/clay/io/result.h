#pragma once

// Error discipline for io: result codes, never exceptions (they cannot cross
// the ABI, and the core builds with -fno-exceptions).

#include <cstdint>
#include <string>

namespace clay {
namespace io {

enum class IoError : std::uint8_t {
    Ok = 0,
    FileNotFound,
    ReadFailed,
    WriteFailed,
    Malformed,        // parse error / truncated / inconsistent
    ForwardVersion,   // file written by a newer major version
    BudgetExceeded,   // triangle/vertex budget guardrail hit
    Unsupported,
};

struct IoStatus {
    IoError error = IoError::Ok;
    std::string detail;

    bool ok() const { return error == IoError::Ok; }
    static IoStatus success() { return {}; }
    static IoStatus fail(IoError e, std::string d = {}) { return {e, std::move(d)}; }
};

// Import guardrails (file-io spec): loaders validate declared counts against
// actual payload size BEFORE allocating.
struct ImportBudget {
    std::size_t max_vertices = 50u * 1000 * 1000;
    std::size_t max_triangles = 100u * 1000 * 1000;
};

}  // namespace io
}  // namespace clay
