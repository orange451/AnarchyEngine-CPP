#include "PresentClock.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#endif

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifdef __APPLE__
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#include <CoreVideo/CVDisplayLink.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace {

GLFWmonitor* MonitorCovering(GLFWwindow* window) {
    if (GLFWmonitor* exclusive = glfwGetWindowMonitor(window)) {
        return exclusive;
    }

    int windowX = 0;
    int windowY = 0;
    int windowWidth = 0;
    int windowHeight = 0;
    glfwGetWindowPos(window, &windowX, &windowY);
    glfwGetWindowSize(window, &windowWidth, &windowHeight);

    int count = 0;
    GLFWmonitor** monitors = glfwGetMonitors(&count);
    GLFWmonitor* best = glfwGetPrimaryMonitor();
    int bestArea = 0;
    for (int i = 0; i < count; ++i) {
        int monitorX = 0;
        int monitorY = 0;
        glfwGetMonitorPos(monitors[i], &monitorX, &monitorY);
        const GLFWvidmode* mode = glfwGetVideoMode(monitors[i]);
        if (mode == nullptr || mode->width <= 0 || mode->height <= 0) {
            continue;
        }
        const int left = std::max(windowX, monitorX);
        const int top = std::max(windowY, monitorY);
        const int right = std::min(windowX + windowWidth, monitorX + mode->width);
        const int bottom = std::min(windowY + windowHeight, monitorY + mode->height);
        const int area = std::max(0, right - left) * std::max(0, bottom - top);
        if (area > bestArea) {
            bestArea = area;
            best = monitors[i];
        }
    }
    return best;
}

double RefreshPeriod(GLFWmonitor* monitor) {
    if (monitor != nullptr) {
        if (const GLFWvidmode* mode = glfwGetVideoMode(monitor)) {
            if (mode->refreshRate > 0) {
                return 1.0 / static_cast<double>(mode->refreshRate);
            }
        }
    }
    return 1.0 / 60.0;
}

}  // namespace

struct PresentClock::Impl {
#ifdef __APPLE__
    // Drawing stays on the thread that owns the OpenGL context. This records the blank.
    static CVReturn Callback(CVDisplayLinkRef, const CVTimeStamp*, const CVTimeStamp*, CVOptionFlags, CVOptionFlags*,
                             void* context) {
        auto* impl = static_cast<Impl*>(context);
        impl->serial.fetch_add(1, std::memory_order_release);
        glfwPostEmptyEvent();
        return kCVReturnSuccess;
    }

    void stop() {
        if (link == nullptr) {
            return;
        }
        CVDisplayLinkStop(link);
        CVDisplayLinkRelease(link);
        link = nullptr;
    }

    void attach(CGDirectDisplayID next) {
        if (next == kCGNullDirectDisplay || next == failedDisplay) {
            return;
        }
        if (link != nullptr) {
            if (next == display) {
                return;
            }
            if (CVDisplayLinkSetCurrentCGDisplay(link, next) == kCVReturnSuccess) {
                display = next;
            }
            return;
        }

        CVDisplayLinkRef created = nullptr;
        if (CVDisplayLinkCreateWithCGDisplay(next, &created) != kCVReturnSuccess || created == nullptr) {
            failedDisplay = next;
        } else if (CVDisplayLinkSetOutputCallback(created, &Impl::Callback, this) != kCVReturnSuccess ||
                   CVDisplayLinkStart(created) != kCVReturnSuccess) {
            CVDisplayLinkRelease(created);
            failedDisplay = next;
        } else {
            link = created;
            display = next;
            return;
        }

        if (!warned) {
            std::fprintf(stderr, "Display link unavailable. Presents follow a %.0f Hz timer.\n", 1.0 / period);
            warned = true;
        }
    }

    CVDisplayLinkRef link = nullptr;
    std::atomic<std::uint64_t> serial{0};
    std::uint64_t acknowledged = 0;
    CGDirectDisplayID display = kCGNullDirectDisplay;
    CGDirectDisplayID failedDisplay = kCGNullDirectDisplay;
    bool warned = false;
    NSTimer* resizeTimer = nil;
    id observedWindow = nil;

    double nominalPeriod() const;
    void deliverResizeBlank();
    void stopResizeTimer();
    void installResizeTimer();
    void unwatchResize();
    void watchResize(GLFWwindow* window);
#endif
    void (*resizePresent)(void*) = nullptr;
    void* resizeContext = nullptr;
    double period = 1.0 / 60.0;
    double nextPresent = 0.0;
};

#ifdef __APPLE__

double PresentClock::Impl::nominalPeriod() const {
    if (link != nullptr) {
        const CVTime time = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link);
        if ((time.flags & kCVTimeIsIndefinite) == 0 && time.timeScale > 0 && time.timeValue > 0) {
            return static_cast<double>(time.timeValue) / static_cast<double>(time.timeScale);
        }
    }
    return period > 0.0 ? period : 1.0 / 60.0;
}

// The border drag runs inside a tracking loop, so waitForNewBlank does not
// return. This timer is armed for the life of the window and only runs in that
// mode. A swap from the loop stays off screen until AppKit displays the view.
void PresentClock::Impl::deliverResizeBlank() {
    if (resizePresent == nullptr) {
        return;
    }
    if (link != nullptr) {
        const std::uint64_t serialNow = serial.load(std::memory_order_acquire);
        if (serialNow == acknowledged) {
            return;
        }
        if (observedWindow != nil) {
            NSWindow* window = (NSWindow*)observedWindow;
            [[window contentView] setNeedsDisplay:YES];
            [window displayIfNeeded];
        } else {
            resizePresent(resizeContext);
        }
        acknowledged = serial.load(std::memory_order_acquire);
        return;
    }

    const double now = glfwGetTime();
    if (now + 0.0001 < nextPresent) {
        return;
    }
    nextPresent += period;
    const double after = glfwGetTime();
    if (nextPresent < after) {
        nextPresent = after + period;
    }
    resizePresent(resizeContext);
}

void PresentClock::Impl::stopResizeTimer() {
    if (resizeTimer == nil) {
        return;
    }
    [resizeTimer invalidate];
    [resizeTimer release];
    resizeTimer = nil;
}

void PresentClock::Impl::installResizeTimer() {
    if (resizeTimer != nil) {
        return;
    }

    // Poll twice per refresh so a blank is noticed within half a frame, and
    // draw only when the serial has moved so the window server is not flooded.
    double interval = nominalPeriod() * 0.5;
    if (interval < 0.001) {
        interval = 0.001;
    } else if (interval > 0.008) {
        interval = 0.008;
    }

    Impl* const captured = this;
    NSTimer* timer = [[NSTimer alloc] initWithFireDate:[NSDate distantPast]
                                               interval:interval
                                                repeats:YES
                                                  block:^(NSTimer*) { captured->deliverResizeBlank(); }];
    // Default mode is the normal frame loop. This timer must not run there.
    [[NSRunLoop mainRunLoop] addTimer:timer forMode:NSEventTrackingRunLoopMode];
    resizeTimer = timer;
}

void PresentClock::Impl::unwatchResize() {
    stopResizeTimer();
    observedWindow = nil;
}

void PresentClock::Impl::watchResize(GLFWwindow* window) {
    id nsWindow = glfwGetCocoaWindow(window);
    if (nsWindow == observedWindow && resizeTimer != nil) {
        return;
    }
    unwatchResize();
    if (nsWindow == nil) {
        return;
    }
    observedWindow = nsWindow;
    [(NSWindow*)nsWindow setPreservesContentDuringLiveResize:NO];
    installResizeTimer();
}

#endif

PresentClock::PresentClock(GLFWwindow* window) : impl_(new Impl) {
    impl_->nextPresent = glfwGetTime();
    follow(window);
}

PresentClock::~PresentClock() {
#ifdef __APPLE__
    if (impl_ != nullptr) {
        impl_->unwatchResize();
        impl_->stop();
    }
#endif
    delete impl_;
    impl_ = nullptr;
}

void PresentClock::follow(GLFWwindow* window) {
    GLFWmonitor* monitor = MonitorCovering(window);
    impl_->period = RefreshPeriod(monitor);
#ifdef __APPLE__
    if (monitor != nullptr) {
        impl_->attach(glfwGetCocoaMonitor(monitor));
    }
    impl_->watchResize(window);
#endif
}

void PresentClock::setResizePresent(void (*present)(void*), void* context) {
    impl_->resizePresent = present;
    impl_->resizeContext = context;
}

bool PresentClock::waitForNewBlank() {
#ifdef __APPLE__
    if (impl_->link != nullptr) {
        glfwWaitEvents();
        const std::uint64_t serial = impl_->serial.load(std::memory_order_acquire);
        if (serial == impl_->acknowledged) {
            return false;
        }
        impl_->acknowledged = serial;
        return true;
    }
#endif

    const double now = glfwGetTime();
    const double remaining = impl_->nextPresent - now;
    if (remaining > 0.0) {
        glfwWaitEventsTimeout(remaining);
    } else {
        glfwPollEvents();
    }
    const double after = glfwGetTime();
    if (after + 0.0001 < impl_->nextPresent) {
        return false;
    }
    impl_->nextPresent += impl_->period;
    if (impl_->nextPresent < after) {
        impl_->nextPresent = after + impl_->period;
    }
    return true;
}

void PresentClock::acknowledge() {
#ifdef __APPLE__
    if (impl_->link != nullptr) {
        impl_->acknowledged = impl_->serial.load(std::memory_order_acquire);
    }
#endif
}
