#include "modernime/core/learning_writer.h"

#include "modernime/core/learning_store.h"

#include <chrono>
#include <utility>
#include <vector>

namespace modernime::core {

LearningWriter::LearningWriter(std::filesystem::path path)
    : store_(std::make_unique<LearningStore>(std::move(path))),
      snapshot_(std::make_shared<const LearningSnapshot>()) {
    storageAvailable_ = store_->open();
    if (storageAvailable_) {
        const auto loaded = store_->snapshot();
        if (loaded != nullptr) {
            snapshot_ = std::move(loaded);
        } else {
            storageAvailable_ = false;
            store_->close();
        }
    }
    worker_ = std::thread(&LearningWriter::run, this);
}

LearningWriter::~LearningWriter() {
    // Give retained events one final bounded persistence cycle before asking
    // the worker to stop. flush() never waits past a completed retry cycle, so
    // a permanently unavailable store still cannot deadlock destruction.
    flush();
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wakeup_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
    store_->close();
}

bool LearningWriter::enqueueSelection(
    std::string_view phrase, std::string_view pinyin,
    std::string_view contextBefore, std::string_view contextAfter,
    std::int64_t nowMs) {
    std::lock_guard lock(mutex_);
    auto next = std::make_shared<LearningSnapshot>(*snapshot_);
    next->recordSelection(phrase, pinyin, contextBefore, contextAfter, nowMs);
    snapshot_ = std::move(next);
    return enqueueForPersistence(
        {EventKind::Selection, std::string(phrase), std::string(pinyin),
         std::string(contextBefore), std::string(contextAfter), nowMs});
}

bool LearningWriter::enqueueNegativeFeedback(std::string_view phrase,
                                              std::string_view pinyin) {
    std::lock_guard lock(mutex_);
    auto next = std::make_shared<LearningSnapshot>(*snapshot_);
    next->recordNegativeFeedback(phrase, pinyin);
    snapshot_ = std::move(next);
    return enqueueForPersistence({EventKind::NegativeFeedback,
                                  std::string(phrase), std::string(pinyin),
                                  {}, {}, 0});
}

bool LearningWriter::enqueueSuppression(std::string_view phrase,
                                        std::string_view pinyin) {
    std::lock_guard lock(mutex_);
    auto next = std::make_shared<LearningSnapshot>(*snapshot_);
    next->recordSuppression(phrase, pinyin);
    snapshot_ = std::move(next);
    return enqueueForPersistence({EventKind::Suppression, std::string(phrase),
                                  std::string(pinyin), {}, {}, 0});
}

bool LearningWriter::enqueueForPersistence(Event event) {
    const bool durablyAvailable = storageAvailable_;
    if (stopping_ ||
        (!durablyAvailable && !processing_ && events_.empty())) {
        return false;
    }
    events_.push_back(std::move(event));
    if (!durablyAvailable && retryExhausted_ && !processing_) {
        retryExhausted_ = false;
        automaticRetryCyclesRemaining_ = 1;
    }
    wakeup_.notify_one();
    return durablyAvailable;
}

bool LearningWriter::flush() {
    std::unique_lock lock(mutex_);
    if (!events_.empty() && retryExhausted_ && !processing_) {
        retryExhausted_ = false;
        automaticRetryCyclesRemaining_ = 1;
        wakeup_.notify_one();
    }
    drained_.wait(lock, [this] {
        return !processing_ && (events_.empty() || retryExhausted_);
    });
    return storageAvailable_ && events_.empty();
}

std::shared_ptr<const LearningSnapshot> LearningWriter::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

bool LearningWriter::persistBatch(const std::vector<Event> &events) {
    constexpr int kMaxAttemptsPerCycle = 3;
    std::vector<LearningEvent> storeEvents;
    storeEvents.reserve(events.size());
    for (const auto &event : events) {
        const auto kind = event.kind == EventKind::Selection
                              ? LearningEvent::Kind::Selection
                              : event.kind == EventKind::NegativeFeedback
                                    ? LearningEvent::Kind::NegativeFeedback
                                    : LearningEvent::Kind::Suppression;
        storeEvents.push_back({kind, event.phrase, event.pinyin,
                               event.contextBefore, event.contextAfter,
                               event.nowMs});
    }

    for (int attempt = 0; attempt < kMaxAttemptsPerCycle; ++attempt) {
        if (!store_->open()) {
            store_->close();
            continue;
        }
        if (store_->recordBatch(storeEvents)) {
            return true;
        }
        store_->close();
    }
    return false;
}

void LearningWriter::run() {
    while (true) {
        std::vector<Event> events;
        {
            std::unique_lock lock(mutex_);
            if (!stopping_ && retryExhausted_ && !events_.empty() &&
                automaticRetryCyclesRemaining_ > 0) {
                wakeup_.wait_for(lock, std::chrono::milliseconds(250),
                                 [this] {
                                     return stopping_ || !retryExhausted_;
                                 });
                if (!stopping_ && retryExhausted_) {
                    retryExhausted_ = false;
                    --automaticRetryCyclesRemaining_;
                }
            }
            wakeup_.wait(lock, [this] {
                return stopping_ || (!events_.empty() && !retryExhausted_);
            });
            if (stopping_ && (events_.empty() || retryExhausted_)) {
                return;
            }
            events.reserve(events_.size());
            while (!events_.empty()) {
                events.push_back(std::move(events_.front()));
                events_.pop_front();
            }
            processing_ = true;
        }

        const bool success = persistBatch(events);

        {
            std::lock_guard lock(mutex_);
            if (!success) {
                events_.insert(events_.begin(),
                               std::make_move_iterator(events.begin()),
                               std::make_move_iterator(events.end()));
                storageAvailable_ = false;
                retryExhausted_ = true;
            } else {
                storageAvailable_ = true;
                retryExhausted_ = false;
                automaticRetryCyclesRemaining_ = 1;
            }
            processing_ = false;
            drained_.notify_all();
        }
    }
}

} // namespace modernime::core
