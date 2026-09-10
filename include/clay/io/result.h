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
    // BOTH OF THESE ARE "MORE THAN ANY REAL ASSET HAS", not "where the engine
    // stops working" -- and which one a limit is decides whether raising it for
    // a genuine case is safe or a bug, so it is written down (issue #510).
    //
    // The structural ceiling is far above both: `mesh::Mesh::indices` is
    // `std::uint32_t`, so 4,294,967,295 is where indexing actually breaks.
    // 50M vertices is 86x under that, and 100M triangles is 300M indices,
    // 14.3x under it. Neither number is protecting an invariant.
    //
    // What they protect is the ALLOCATION, which is why the loaders check them
    // against a DECLARED count before reading a payload -- ply.cpp calls the
    // check right below it an "allocation-bomb guardrail". A header claiming
    // 50M vertices costs 0.60 GB in positions alone and 2.20 GB carrying
    // normals, colours and uvs; 100M triangles is 1.20 GB of indices. A
    // malformed or hostile header should be refused rather than believed.
    //
    // SO RAISING EITHER FOR A GENUINE ASSET IS SAFE, up to what the machine can
    // hold and well short of the uint32 ceiling. Lowering them is also safe.
    // What is not safe is raising them past 4.29 billion, where the index type
    // itself gives out -- and that is a different conversation, because it
    // changes a type rather than a policy.
    std::size_t max_vertices = 50u * 1000 * 1000;
    std::size_t max_triangles = 100u * 1000 * 1000;
    // Ceiling on the bytes a *_file loader will read into memory. It is checked
    // before the buffer is sized, so a length that is not a real file length —
    // a directory tells LONG_MAX on glibc — is refused rather than allocated.
    std::size_t max_file_bytes = 2ull * 1024 * 1024 * 1024;
};

}  // namespace io
}  // namespace clay
