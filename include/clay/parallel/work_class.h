#pragma once

// WHAT A PIECE OF WORK IS FOR, so the platform can schedule it against the
// other things the device is doing.
//
// The pool below this had one speed. On a desktop that is nearly right: the
// cores are interchangeable and the scheduler has little to decide. On a phone
// or tablet it is wrong in a way that shows up as latency and nothing else —
// fast and efficiency cores, a scheduler that reads a thread's declared intent,
// and a user whose pencil is waiting on whichever core the OS felt like giving
// the job. Maintenance work and a brush dab arriving as indistinguishable
// threads means the dab can wait behind the maintenance.
//
// THIS IS NOT APPLE'S ENUM. The names here say what the work IS, not which
// `QOS_CLASS_*` it maps to, because a portable core that spells out a vendor's
// scheduling constants in its public headers has to keep spelling them out on
// every other platform that does not have them. The mapping lives in one
// function in one file, and everywhere else this is the vocabulary.
//
// Off Apple this is currently a no-op that records the class and returns —
// stated in code rather than silently, so a reader can see the seam is
// deliberate and where a Windows or Linux policy would go.

#include <cstdint>

namespace clay {
namespace parallel {

enum class WorkClass : std::uint8_t {
    // A user is waiting on this frame. Brush dabs, the picking query behind a
    // cursor, anything between an input event and the pixels answering it.
    Interactive,
    // A user asked for it and is waiting, but not within a frame: a mesh
    // export, a remesh of a whole surface, an explicit long operation.
    UserInitiated,
    // Housekeeping the user did not ask for and will not wait for. Cache
    // maintenance, a BVH rebuild between strokes, trimming.
    Utility,
    // Genuinely deferrable. Runs when nothing else wants the core.
    Background,
};

namespace detail {
// What this thread is running as. Thread-local because that is exactly the
// scope the platform APIs operate on: a QoS class is a property of a thread,
// not of a job object.
inline WorkClass& current_slot() noexcept {
    static thread_local WorkClass cls = WorkClass::UserInitiated;
    return cls;
}
}  // namespace detail

// The class this thread is currently running as.
//
// Readable on every platform, including the ones where applying a class does
// nothing. That is deliberate: it makes the propagation rules — a worker adopts
// its job's class, a nested call inherits its caller's — testable off Apple,
// where the platform call itself has nothing to introspect.
inline WorkClass current_work_class() noexcept { return detail::current_slot(); }

// Ask the platform to schedule THIS thread according to `cls`.
//
// Defined in src/parallel/thread_policy.cpp. Apple maps it onto the QoS
// classes; everywhere else it does nothing yet, and the no-op is a written
// branch rather than an absent file.
void apply_platform_work_class(WorkClass cls) noexcept;

// Record `cls` for this thread and hand it to the platform.
inline void apply_work_class(WorkClass cls) noexcept {
    detail::current_slot() = cls;
    apply_platform_work_class(cls);
}

// Applies a class for a scope and RESTORES WHAT WAS THERE, rather than clearing
// to a default. The pool's workers are persistent: a worker that ran an
// Interactive job and then reset to "none" would carry that into the next job
// it picks up, and a nested inline call must not tell the frame above it that
// the class has changed.
class WorkClassScope {
  public:
    explicit WorkClassScope(WorkClass cls) noexcept : previous_(current_work_class()) {
        apply_work_class(cls);
    }
    ~WorkClassScope() { apply_work_class(previous_); }
    WorkClassScope(const WorkClassScope&) = delete;
    WorkClassScope& operator=(const WorkClassScope&) = delete;

  private:
    WorkClass previous_;
};

}  // namespace parallel
}  // namespace clay
