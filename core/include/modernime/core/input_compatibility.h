#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace modernime::core {

struct InputCompatibilityFacts final {
    std::string application;
    std::string frontend;
    std::string display;
    bool focused = false;
    bool sensitive = false;
    bool surroundingText = false;
    bool clientPreedit = false;
    bool cursorRectangle = false;
    bool modernimeSelected = false;
    std::uint64_t observedAtMs = 0;
};

struct InputCompatibilityReport final {
    bool observed = false;
    InputCompatibilityFacts facts;
    std::string transport = "unknown";
    std::string toolkit = "unknown";
    std::string sandbox = "unknown";
    std::uint64_t handledKeys = 0;
    std::uint64_t maximumDispatchUs = 0;
    std::vector<std::string> suggestions;
};

// Stores only bounded host facts and timing counters, never input text.
// The adapter is the only observer; management callers read snapshots.
class InputCompatibilityTracker final {
public:
    void observe(InputCompatibilityFacts facts);
    void recordKey(const std::string &application, bool handled,
                   std::uint64_t dispatchUs);
    InputCompatibilityReport report(const std::string &application = {}) const;
    std::vector<std::string> applications() const;
private:
    mutable std::mutex mutex_;
    std::map<std::string, InputCompatibilityReport> reports_;
    std::string latest_;
};

} // namespace modernime::core
