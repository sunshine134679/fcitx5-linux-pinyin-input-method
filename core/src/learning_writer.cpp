#include "modernime/core/learning_writer.h"

#include "modernime/core/english_dictionary.h"
#include "modernime/core/learning_policy.h"
#include "modernime/core/learning_store.h"
#include "modernime/core/pinyin_match.h"

#include <chrono>
#include <utility>
#include <vector>

namespace modernime::core {

LearningWriter::LearningWriter(std::filesystem::path path)
    : store_(std::make_unique<LearningStore>(path)),
      observerStore_(std::make_unique<LearningStore>(std::move(path))),
      snapshot_(std::make_shared<const LearningSnapshot>()) {
    storageAvailable_ = store_->open();
    if (storageAvailable_) {
        const auto loaded = store_->readState();
        if (loaded.has_value()) {
            snapshot_ = loaded->snapshot;
            resetRevision_ = loaded->resetRevision;
        } else {
            storageAvailable_ = false;
            store_->close();
        }
    }
    observerStore_->openReadOnly();
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
    refreshBeforeEnqueue();
    std::lock_guard lock(mutex_);
    return enqueueSelectionLocked(phrase, pinyin, contextBefore, contextAfter, nowMs);
}

bool LearningWriter::enqueueSelectionLocked(
    std::string_view phrase, std::string_view pinyin,
    std::string_view contextBefore, std::string_view contextAfter,
    std::int64_t nowMs) {
    auto next = std::make_shared<LearningSnapshot>(*snapshot_);
    next->recordSelection(phrase, pinyin, contextBefore, contextAfter, nowMs);
    snapshot_ = std::move(next);
    ++snapshotVersion_;
    return enqueueForPersistence(
        {EventKind::Selection, std::string(phrase), std::string(pinyin),
         std::string(contextBefore), std::string(contextAfter), nowMs});
}

bool LearningWriter::enqueueCommit(const CommitEvent &event) {
    const auto pinyinKey = event.fullPinyin.empty() ? event.text : event.fullPinyin;

    refreshBeforeEnqueue();
    std::lock_guard lock(mutex_);

    const bool hasPositive = snapshot_->hasPositiveFrequency(event.text, pinyinKey);
    const auto decision = decideCommitLearning(event, hasPositive);
    if (decision != CommitLearningDecision::Record) {
        // Skipped successfully without error; returning true maintains compatibility
        // with callers that check whether the commit was handled.
        return true;
    }

    return enqueueSelectionLocked(event.text, pinyinKey, event.contextBefore,
                                  event.contextAfter, event.timestampMs);
}

bool LearningWriter::enqueueNegativeFeedback(std::string_view phrase,
                                              std::string_view pinyin) {
    refreshBeforeEnqueue();
    std::lock_guard lock(mutex_);
    auto next = std::make_shared<LearningSnapshot>(*snapshot_);
    next->recordNegativeFeedback(phrase, pinyin);
    snapshot_ = std::move(next);
    ++snapshotVersion_;
    return enqueueForPersistence({EventKind::NegativeFeedback,
                                  std::string(phrase), std::string(pinyin),
                                  {}, {}, 0});
}

bool LearningWriter::enqueueSuppression(std::string_view phrase,
                                        std::string_view pinyin) {
    refreshBeforeEnqueue();
    std::lock_guard lock(mutex_);
    auto next = std::make_shared<LearningSnapshot>(*snapshot_);
    next->recordSuppression(phrase, pinyin);
    snapshot_ = std::move(next);
    ++snapshotVersion_;
    return enqueueForPersistence({EventKind::Suppression, std::string(phrase),
                                  std::string(pinyin), {}, {}, 0});
}

bool LearningWriter::enqueueForPersistence(Event event) {
    const bool durablyAvailable = storageAvailable_;
    if (stopping_) {
        return false;
    }
    event.resetRevision = resetRevision_;
    events_.push_back(std::move(event));
    if (!durablyAvailable) {
        while (events_.size() > kMaxQueuedEvents) {
            events_.pop_front();
        }
    }
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

bool LearningWriter::reload() {
    flush();
    std::lock_guard storeLock(storeMutex_);
    if (!store_->open()) {
        store_->close();
        return false;
    }
    const auto loaded = store_->readState();
    if (!loaded.has_value()) {
        return false;
    }
    std::lock_guard lock(mutex_);
    rebaseSnapshot(*loaded, true);
    storageAvailable_ = true;
    // A refreshed store starts a new bounded retry cycle. Otherwise a queue
    // exhausted before the clear would also leave new selections sleeping.
    retryExhausted_ = false;
    automaticRetryCyclesRemaining_ = 1;
    wakeup_.notify_one();
    return true;
}

bool LearningWriter::reloadIfChanged() {
    // Do not wait for the asynchronous writer, or reopen its failed writable
    // connection, on the Fcitx input event loop.
    std::unique_lock storeLock(storeMutex_, std::try_to_lock);
    if (!storeLock.owns_lock()) {
        return false;
    }
    std::lock_guard observerLock(observerMutex_);
    if (!observerStore_->openReadOnly()) {
        return false;
    }
    const auto revision = observerStore_->resetRevision();
    {
        std::lock_guard lock(mutex_);
        if (!revision.has_value() || *revision <= resetRevision_) {
            return false;
        }
    }
    const auto loaded = observerStore_->readState();
    if (!loaded.has_value()) {
        return false;
    }
    std::lock_guard lock(mutex_);
    rebaseSnapshot(*loaded, true);
    storageAvailable_ = true;
    retryExhausted_ = false;
    automaticRetryCyclesRemaining_ = 1;
    wakeup_.notify_one();
    return true;
}

void LearningWriter::refreshBeforeEnqueue() {
    // Read the clear generation before tagging new input. Otherwise the first
    // selection after clear, but before the next timer tick, looks pre-clear.
    std::lock_guard observerLock(observerMutex_);
    if (!observerStore_->openReadOnly()) {
        return;
    }
    const auto revision = observerStore_->resetRevision();
    {
        std::lock_guard lock(mutex_);
        if (!revision.has_value() || *revision <= resetRevision_) {
            return;
        }
    }
    const auto loaded = observerStore_->readState();
    if (!loaded.has_value()) {
        return;
    }
    std::lock_guard lock(mutex_);
    if (loaded->resetRevision > resetRevision_) {
        rebaseSnapshot(*loaded, true);
        storageAvailable_ = true;
        retryExhausted_ = false;
        automaticRetryCyclesRemaining_ = 1;
        wakeup_.notify_one();
    }
}

void LearningWriter::applyEvent(LearningSnapshot &snapshot, const Event &event) {
    switch (event.kind) {
    case EventKind::Selection:
        snapshot.recordSelection(event.phrase, event.pinyin, event.contextBefore,
                                 event.contextAfter, event.nowMs);
        break;
    case EventKind::NegativeFeedback:
        snapshot.recordNegativeFeedback(event.phrase, event.pinyin);
        break;
    case EventKind::Suppression:
        snapshot.recordSuppression(event.phrase, event.pinyin);
        break;
    }
}

void LearningWriter::rebaseSnapshot(const LearningStoreState &state,
                                   bool includeProcessing) {
    // A clear may be observed on the input thread after the worker read its
    // snapshot. Never publish that older generation over the refreshed cache.
    if (state.resetRevision < resetRevision_) {
        return;
    }
    if (state.resetRevision > resetRevision_) {
        std::erase_if(events_, [rev = state.resetRevision](const Event &event) {
            return event.resetRevision < rev;
        });
        if (includeProcessing) {
            std::erase_if(processingEvents_, [rev = state.resetRevision](const Event &event) {
                return event.resetRevision < rev;
            });
        }
    }
    auto next = std::make_shared<LearningSnapshot>(*state.snapshot);
    const auto apply = [&](const auto &events) {
        for (const auto &event : events) {
            if (event.resetRevision == state.resetRevision) {
                applyEvent(*next, event);
            }
        }
    };
    if (includeProcessing) {
        apply(processingEvents_);
    }
    apply(events_);
    resetRevision_ = state.resetRevision;
    snapshot_ = std::move(next);
    ++snapshotVersion_;
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
                               event.nowMs, event.resetRevision});
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
            processingEvents_ = events;
        }

        bool success = false;
        std::optional<LearningStoreState> freshState;
        {
            std::lock_guard storeLock(storeMutex_);
            success = persistBatch(events);
            freshState = success ? store_->readState() : std::nullopt;
        }

        // Replay pending input without holding the input mutex. If another
        // enqueue/clear changes the version, its optimistic snapshot remains
        // authoritative and the next batch can publish a fresh rebase.
        std::shared_ptr<LearningSnapshot> prepared;
        std::uint64_t capturedVersion = 0;
        if (success && freshState) {
            std::vector<Event> pending;
            {
                std::lock_guard lock(mutex_);
                capturedVersion = snapshotVersion_;
                pending.assign(events_.begin(), events_.end());
            }
            prepared = std::make_shared<LearningSnapshot>(*freshState->snapshot);
            for (const auto &event : pending) {
                if (event.resetRevision == freshState->resetRevision) applyEvent(*prepared, event);
            }
        }

        // Release obsolete vocabulary allocations after unlocking; enqueue
        // must not wait for the destruction of a large retired snapshot.
        std::shared_ptr<const LearningSnapshot> retiredSnapshot;
        {
            std::lock_guard lock(mutex_);
            if (!success) {
                events_.insert(events_.begin(),
                               std::make_move_iterator(events.begin()),
                               std::make_move_iterator(events.end()));
                // Include the in-flight failed batch in the limit. Events
                // arriving during persistence are newer and take priority.
                while (events_.size() > kMaxQueuedEvents) {
                    events_.pop_front();
                }
                storageAvailable_ = false;
                retryExhausted_ = true;
            } else {
                storageAvailable_ = true;
                retryExhausted_ = false;
                automaticRetryCyclesRemaining_ = 1;
                if (freshState && freshState->resetRevision > resetRevision_) {
                    // A previously unseen clear takes precedence even when
                    // input arrived during preparation. Every queued event
                    // still has the older generation and must be discarded.
                    std::erase_if(events_, [rev = freshState->resetRevision](const Event &event) {
                        return event.resetRevision < rev;
                    });
                    retiredSnapshot = std::move(snapshot_);
                    snapshot_ = freshState->snapshot;
                    resetRevision_ = freshState->resetRevision;
                    ++snapshotVersion_;
                } else if (freshState && prepared &&
                           freshState->resetRevision == resetRevision_ &&
                           capturedVersion == snapshotVersion_) {
                    retiredSnapshot = std::move(snapshot_);
                    snapshot_ = std::move(prepared);
                    ++snapshotVersion_;
                }
            }
            processingEvents_.clear();
            processing_ = false;
            drained_.notify_all();
        }
    }
}

} // namespace modernime::core
