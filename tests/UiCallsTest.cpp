#include "ide/UiCalls.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// The MCP server's calls into the UI thread, and how closing the studio ends them.
namespace {

int gFailures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL %s\n", message);
        ++gFailures;
    }
}

using Clock = std::chrono::steady_clock;

// What a call threw, or empty when it returned.
std::string Outcome(const std::function<void()>& call) {
    try {
        call();
    } catch (const std::exception& ex) {
        return ex.what();
    }
    return {};
}

void TestRun() {
    // A UI thread that runs each task at once.
    const ide::UiCalls calls([](std::function<void()> task) { task(); });
    int ran = 0;
    Expect(Outcome([&] { calls.run([&] { ++ran; }, std::chrono::seconds(5)); }).empty() && ran == 1,
           "run returns once the UI thread has run the work");
    Expect(Outcome([&] { calls.run([] { throw std::runtime_error("no Scene View"); }, std::chrono::seconds(5)); }) ==
               "no Scene View",
           "run passes on what the work threw");

    // A UI thread that never gets to the task.
    const ide::UiCalls stuck([](std::function<void()>) {});
    const Clock::time_point start = Clock::now();
    Expect(Outcome([&] { stuck.run([] {}, std::chrono::milliseconds(50)); }) ==
               "The studio did not respond within 50 ms.",
           "a run the UI thread never gets to times out");
    Expect(Outcome([&] { stuck.run([] {}, std::chrono::milliseconds(1000)); }) ==
               "The studio did not respond within 1 second.",
           "the timeout says how long it waited");
    Expect(Clock::now() - start < std::chrono::seconds(2), "the timeout is the wait given");
}

void TestCloseWakesWaits() {
    std::atomic<int> posted{0};
    const ide::UiCalls calls([&](std::function<void()>) { ++posted; });
    std::string waiting;
    std::string waiting_for_paint;
    Clock::duration took{};
    std::thread server([&] {
        const Clock::time_point start = Clock::now();
        waiting = Outcome([&] { calls.run([] {}, std::chrono::seconds(10)); });
        waiting_for_paint = Outcome([&] { calls.wait_until([] { return false; }, std::chrono::seconds(10)); });
        took = Clock::now() - start;
    });
    while (posted.load() == 0) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    calls.close();
    server.join();
    Expect(waiting == "The studio is closing.", "closing ends a call waiting for the UI thread");
    Expect(waiting_for_paint == "The studio is closing.", "a wait that starts after closing ends at once");
    Expect(took < std::chrono::seconds(2), "closing does not wait out the timeouts");
    Expect(Outcome([&] { calls.run([] {}, std::chrono::seconds(10)); }) == "The studio is closing.",
           "a call after closing is refused");
    Expect(posted.load() == 1, "a refused call posts nothing to the UI thread");
}

void TestWaitUntil() {
    const ide::UiCalls calls([](std::function<void()> task) { task(); });
    bool painted = false;
    std::thread paint([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        calls.update([&] { painted = true; });
    });
    bool ready = false;
    Expect(Outcome([&] { ready = calls.wait_until([&] { return painted; }, std::chrono::seconds(5)); }).empty() &&
               ready,
           "wait_until sees a change made through update");
    paint.join();
    Expect(!calls.wait_until([] { return false; }, std::chrono::milliseconds(20)), "wait_until is false when time runs out");
}

}  // namespace

int RunUiCallsTests() {
    gFailures = 0;
    TestRun();
    TestCloseWakesWaits();
    TestWaitUntil();
    if (gFailures == 0) {
        std::printf("ui call tests passed\n");
    }
    return gFailures;
}
