#pragma once

#include "modernime/core/learning_snapshot.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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
    // Re-reads the snapshot from the underlying persistent store.
    bool reload();
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

    bool enqueueForPersistence(Event event);
    bool persistBatch(const std::vector<Event> &events);
    void run();

    // 单个批次写失败后的退避重试间隔；初始 2 秒，翻倍至 30 秒上限。
    static constexpr std::chrono::seconds kInitialBackoff{2};
    static constexpr std::chrono::seconds kMaxBackoff{30};
    // 待落库事件队列上限；超出时丢弃最旧事件，防止磁盘持续故障时无界增长。
    static constexpr std::size_t kMaxQueuedEvents = 100;

    std::unique_ptr<LearningStore> store_;
    mutable std::mutex mutex_;
    std::condition_variable wakeup_;
    std::condition_variable drained_;
    std::deque<Event> events_;
    // Copy-on-write: enqueue replaces this pointer with a mutated clone, so
    // snapshots handed out by snapshot() stay immutable even if a future
    // caller reads them from another thread.
    std::shared_ptr<const LearningSnapshot> snapshot_;
    bool stopping_ = false;
    bool processing_ = false;
    // A failed batch stays queued. flush() explicitly starts the next bounded
    // retry cycle, which gives callers a deterministic failure result instead
    // of either dropping the batch or waiting forever.
    bool retryExhausted_ = false;
    int automaticRetryCyclesRemaining_ = 1;
    bool storageAvailable_ = false;
    std::thread worker_;
};

} // namespace modernime::core
