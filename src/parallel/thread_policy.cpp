// THE ONE PLACE THAT KNOWS WHAT A PLATFORM CALLS A SCHEDULING CLASS.
//
// The guide this implements proposed an Objective-C++ `.mm` for the Apple half.
// It does not need one: `pthread_set_qos_class_self_np` is a plain C function
// in <pthread/qos.h>, so a single translation unit with one `#if` serves both
// halves and the build does not have to enable Objective-C++ to get it.

#include "clay/parallel/work_class.h"

#if defined(__APPLE__)
#include <pthread/qos.h>
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

#else

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
