#include "modernime/core/learning_writer.h"

#include "modernime/core/learning_store.h"

#include <vector>
#include <utility>

namespace modernime::core {

LearningWriter::LearningWriter(std::filesystem::path path)
    : store_(std::make_unique<LearningStore>(std::move(path))),
      snapshot_(std::make_shared<LearningSnapshot>()) {
    storageAvailable_ = store_->open();
    if (storageAvailable_) {
        const auto loaded = store_->snapshot();
        *snapshot_ = *loaded;
    }
    worker_ = std::thread(&LearningWriter::run, this);
}

LearningWriter::~LearningWriter() {
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
    snapshot_->recordSelection(phrase, pinyin, contextBefore, contextAfter,
                               nowMs);
    if (!storageAvailable_ || stopping_) {
        return false;
    }
    events_.push({EventKind::Selection, std::string(phrase),
                  std::string(pinyin), std::string(contextBefore),
                  std::string(contextAfter), nowMs});
    wakeup_.notify_one();
    return true;
}

bool LearningWriter::enqueueNegativeFeedback(std::string_view phrase,
                                              std::string_view pinyin) {
    std::lock_guard lock(mutex_);
    snapshot_->recordNegativeFeedback(phrase, pinyin);
    if (!storageAvailable_ || stopping_) {
        return false;
    }
    events_.push({EventKind::NegativeFeedback, std::string(phrase),
                  std::string(pinyin), {}, {}, 0});
    wakeup_.notify_one();
    return true;
}

bool LearningWriter::flush() {
    std::unique_lock lock(mutex_);
    drained_.wait(lock, [this] {
        return events_.empty() && !processing_;
    });
    return storageAvailable_;
}

std::shared_ptr<const LearningSnapshot> LearningWriter::snapshot() const {
    std::lock_guard lock(mutex_);
    return std::shared_ptr<const LearningSnapshot>(snapshot_);
}

void LearningWriter::run() {
    while (true) {
        std::vector<Event> events;
        {
            std::unique_lock lock(mutex_);
            wakeup_.wait(lock, [this] {
                return stopping_ || !events_.empty();
            });
            if (stopping_ && events_.empty()) {
                return;
            }
            events.reserve(events_.size());
            while (!events_.empty()) {
                events.push_back(std::move(events_.front()));
                events_.pop();
            }
            processing_ = true;
        }

        std::vector<LearningEvent> storeEvents;
        storeEvents.reserve(events.size());
        for (const auto &event : events) {
            storeEvents.push_back({
                event.kind == EventKind::Selection
                    ? LearningEvent::Kind::Selection
                    : LearningEvent::Kind::NegativeFeedback,
                event.phrase, event.pinyin, event.contextBefore,
                event.contextAfter, event.nowMs});
        }
        const bool success = store_->recordBatch(storeEvents);

        {
            std::lock_guard lock(mutex_);
            if (!success) {
                storageAvailable_ = false;
                while (!events_.empty()) {
                    events_.pop();
                }
            }
            processing_ = false;
            if (events_.empty()) {
                drained_.notify_all();
            }
        }
    }
}

} // namespace modernime::core
