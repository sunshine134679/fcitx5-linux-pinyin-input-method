#include "modernime/core/learning_writer.h"

#include "modernime/core/learning_store.h"

#include <utility>

namespace modernime::core {

LearningWriter::LearningWriter(std::filesystem::path path)
    : store_(std::make_unique<LearningStore>(std::move(path))),
      snapshot_(std::make_shared<LearningSnapshot>()) {
    if (store_->open()) {
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
    events_.push({EventKind::NegativeFeedback, std::string(phrase),
                  std::string(pinyin), {}, {}, 0});
    wakeup_.notify_one();
    return true;
}

void LearningWriter::flush() {
    std::unique_lock lock(mutex_);
    drained_.wait(lock, [this] {
        return events_.empty() && !processing_;
    });
}

std::shared_ptr<const LearningSnapshot> LearningWriter::snapshot() const {
    std::lock_guard lock(mutex_);
    return std::shared_ptr<const LearningSnapshot>(snapshot_);
}

void LearningWriter::run() {
    while (true) {
        Event event;
        {
            std::unique_lock lock(mutex_);
            wakeup_.wait(lock, [this] {
                return stopping_ || !events_.empty();
            });
            if (stopping_ && events_.empty()) {
                return;
            }
            event = std::move(events_.front());
            events_.pop();
            processing_ = true;
        }

        if (event.kind == EventKind::Selection) {
            store_->recordSelection(event.phrase, event.pinyin,
                                    event.contextBefore, event.contextAfter,
                                    event.nowMs);
        } else {
            store_->recordNegativeFeedback(event.phrase, event.pinyin);
        }

        {
            std::lock_guard lock(mutex_);
            processing_ = false;
            if (events_.empty()) {
                drained_.notify_all();
            }
        }
    }
}

} // namespace modernime::core
