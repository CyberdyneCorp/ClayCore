// THE ONE PLACE THAT KNOWS WHAT A PLATFORM CALLS A SCHEDULING CLASS.
//
// The guide this implements proposed an Objective-C++ `.mm` for the Apple half.
// It does not need one: `pthread_set_qos_class_self_np` is a plain C function
// in <pthread/qos.h>, so a single translation unit with one `#if` serves both
// halves and the build does not have to enable Objective-C++ to get it.

#include "clay/parallel/work_class.h"

#include <thread>

#if defined(__APPLE__)
#include <pthread/qos.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

namespace clay {
namespace parallel {

#if defined(__APPLE__)

namespace {
qos_class_t to_qos(WorkClass cls) noexcept {
    switch (cls) {
        // USER_INTERACTIVE is deliberately not used. Apple reserves it for the
        // main thread's event handling, and a pool of worker threads all
        // claiming it competes with the UI thread it is supposed to be feeding.
        // USER_INITIATED is the highest class a worker should take.
        case WorkClass::Interactive:
            return QOS_CLASS_USER_INITIATED;
        case WorkClass::UserInitiated:
            return QOS_CLASS_USER_INITIATED;
        case WorkClass::Utility:
            return QOS_CLASS_UTILITY;
        case WorkClass::Background:
            return QOS_CLASS_BACKGROUND;
    }
    return QOS_CLASS_DEFAULT;
}
}  // namespace

void apply_platform_work_class(WorkClass cls) noexcept {
    // Relative priority 0: the class is the coarse decision and is the one
    // worth making. A negative offset within a class is a tuning knob that
    // wants device measurements behind it, and there are none yet.
    pthread_set_qos_class_self_np(to_qos(cls), 0);
}

// HOW A PLATFORM COUNTS PERFORMANCE CORES (task 1.2).
//
// `hw.perflevel0.logicalcpu` is the count of the FASTEST level. Apple orders
// perflevels fastest-first, so level 0 is P on every SoC that distinguishes
// them and is the whole machine on one that does not -- an Intel Mac has a
// single perflevel, and there the answer is `hardware_concurrency`, which is
// the right answer rather than a fallback.
//
// SYSCTL RATHER THAN A HARDCODED TABLE, because the table would be wrong on the
// next SoC and wrong silently. A device the sysctl does not know is a device
// this returns 0 for, and 0 means "no opinion" to the caller rather than "run
// serially" -- the distinction the fallback below turns on.
std::size_t platform_performance_cores() noexcept {
    std::uint32_t count = 0;
    std::size_t size = sizeof count;
    if (sysctlbyname("hw.perflevel0.logicalcpu", &count, &size, nullptr, 0) == 0 && count > 0)
        return count;
    // The sysctl is absent before macOS 12 / iOS 15 and on Intel. Both are
    // machines whose cores are interchangeable, so every core IS a performance
    // core and hardware_concurrency is the honest count.
    return 0;
}

#else

// NO PLATFORM ANSWER, and that is different from "one core". Linux exposes
// heterogeneity through cpufreq and DT bindings, Windows through
// CPUSETINFORMATION EfficiencyClass, and neither is close enough to Apple's
// perflevel to guess with. Returning 0 says "no opinion" and the caller falls
// back to hardware_concurrency, which is what the pool did before this change.
std::size_t platform_performance_cores() noexcept { return 0; }

void apply_platform_work_class(WorkClass cls) noexcept {
    // NO-OP, AND WRITTEN OUT RATHER THAN ABSENT. Windows thread priorities,
    // Linux nice/sched_setattr and Android's scheduling hints are all real
    // options, and none of them is a faithful equivalent of a QoS class — the
    // feature does not require every platform to have one on day one. The
    // recorded class in `apply_work_class` still holds, so the propagation
    // rules behave identically everywhere and only the OS hint is missing.
    (void)cls;
}

#endif

}  // namespace parallel
}  // namespace clay
