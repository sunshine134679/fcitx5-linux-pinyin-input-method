#pragma once

#include "modernime/core/learning_snapshot.h"

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>

namespace modernime::core {

class LearningStore;

class LearningWriter final {
public:
    explicit LearningWriter(std::filesystem::path path);
    ~LearningWriter();

    LearningWriter(const LearningWriter &) = delete;
    LearningWriter &operator=(const LearningWriter &) = delete;

    bool enqueueSelection(std::string_view phrase, std::string_view pinyin,
                          std::string_view contextBefore,
                          std::string_view contextAfter,
                          std::int64_t nowMs);
    bool enqueueNegativeFeedback(std::string_view phrase,
                                 std::string_view pinyin);
    bool enqueueSuppression(std::string_view phrase,
                            std::string_view pinyin);
    // Wait until queued events have been processed. Returns false when the
    // backing store was unavailable or a write failed.
    bool flush();
    std::shared_ptr<const LearningSnapshot> snapshot() const;

private:
    enum class EventKind { Selection, NegativeFeedback, Suppression };

    struct Event final {
        EventKind kind = EventKind::Selection;
        std::string phrase;
        std::string pinyin;
        std::string contextBefore;
        std::string contextAfter;
        std::int64_t nowMs = 0;
    };

    void run();

    std::unique_ptr<LearningStore> store_;
    mutable std::mutex mutex_;
    std::condition_variable wakeup_;
    std::condition_variable drained_;
    std::queue<Event> events_;
    std::shared_ptr<LearningSnapshot> snapshot_;
    bool stopping_ = false;
    bool processing_ = false;
    bool storageAvailable_ = false;
    std::thread worker_;
};

} // namespace modernime::core
