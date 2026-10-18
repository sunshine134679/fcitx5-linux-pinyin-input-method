#include "modernime/core/learning_writer.h"

#include "modernime/core/english_dictionary.h"
#include "modernime/core/learning_policy.h"
#include "modernime/core/learning_store.h"
#include "modernime/core/pinyin_match.h"

#include <chrono>
#include <utility>
#include <vector>

namespace modernime::core {

namespace {
std::shared_ptr<const LearningSnapshot> emptySnapshot() {
    static const auto kEmpty = std::make_shared<const LearningSnapshot>();
    return kEmpty;
}
} // namespace

LearningWriter::LearningWriter(std::filesystem::path path, bool synchronousInitialLoad,
                               std::size_t totalEntryLimit)
    : store_(std::make_unique<LearningStore>(path)),
      observerStore_(std::make_unique<LearningStore>(std::move(path))),
      totalEntryLimit_(totalEntryLimit),
      snapshot_(emptySnapshot()) {
    if (totalEntryLimit_ > 0) {
        store_->setTotalEntryLimit(totalEntryLimit_);
        observerStore_->setTotalEntryLimit(totalEntryLimit_);
    }
    if (synchronousInitialLoad) {
        storageAvailable_ = store_->open();
        if (storageAvailable_) {
            const auto loaded = store_->readState();
            if (loaded.has_value()) {
                snapshot_ = loaded->snapshot;
                resetRevision_ = loaded->resetRevision;
                viewState_ = EpochViewState::Ready;
            } else {
                storageAvailable_ = false;
                viewState_ = EpochViewState::Reloading;
                retryExhausted_ = true;
                store_->close();
            }
        } else {
            viewState_ = EpochViewState::Reloading;
            retryExhausted_ = true;
        }
    } else {
        viewState_ = EpochViewState::Reloading;
        retryExhausted_ = false;
        automaticRetryCyclesRemaining_ = 1;
        if (observerStore_->openReadOnly()) {
            const auto rev = observerStore_->resetRevision();
            if (rev.has_value()) {
                resetRevision_ = *rev;
                storageAvailable_ = true;
            }
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
        workerPaused_ = false;
        workerPausedAfterRead_ = false;
    }
    resumeWorkerCv_.notify_all();
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
    Event event;
    event.kind = EventKind::Selection;
    event.phrase = phrase;
    event.pinyin = pinyin;
    event.contextBefore = contextBefore;
    event.contextAfter = contextAfter;
    event.nowMs = nowMs;
    return enqueueForPersistence(std::move(event));
}

bool LearningWriter::enqueueCommit(const CommitEvent &event) {
    const auto pinyinKey = event.fullPinyin.empty() ? event.text : event.fullPinyin;

    refreshBeforeEnqueue();
    std::lock_guard lock(mutex_);

    if (viewState_ == EpochViewState::Reloading) {
        if (event.role == CommitCandidateRole::Command) {
            return true;
        }
        if (event.role == CommitCandidateRole::Raw &&
            PinyinMatchPolicy::isValidPinyinSequence(event.text) &&
            !EnglishDictionary::isEnglishWord(event.text)) {
            return true;
        }
        Event e;
        e.kind = EventKind::PendingCommit;
        e.phrase = event.text;
        e.pinyin = pinyinKey;
        e.contextBefore = event.contextBefore;
        e.contextAfter = event.contextAfter;
        e.nowMs = event.timestampMs;
        e.resetRevision = resetRevision_;
        e.commitEvent = event;
        return enqueueForPersistence(std::move(e));
    }

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
    Event event;
    event.kind = EventKind::NegativeFeedback;
    event.phrase = phrase;
    event.pinyin = pinyin;
    return enqueueForPersistence(std::move(event));
}

bool LearningWriter::enqueueSuppression(std::string_view phrase,
                                        std::string_view pinyin) {
    refreshBeforeEnqueue();
    std::lock_guard lock(mutex_);
    auto next = std::make_shared<LearningSnapshot>(*snapshot_);
    next->recordSuppression(phrase, pinyin);
    snapshot_ = std::move(next);
    ++snapshotVersion_;
    Event event;
    event.kind = EventKind::Suppression;
    event.phrase = phrase;
    event.pinyin = pinyin;
    return enqueueForPersistence(std::move(event));
}

bool LearningWriter::enqueueForPersistence(Event event) {
    const bool durablyAvailable = storageAvailable_;
    if (stopping_) {
        return false;
    }
    event.resetRevision = resetRevision_;
    events_.push_back(std::move(event));
    if (!durablyAvailable || viewState_ == EpochViewState::Reloading) {
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
    if ((!events_.empty() || viewState_ == EpochViewState::Reloading) &&
        retryExhausted_ && !processing_) {
        retryExhausted_ = false;
        automaticRetryCyclesRemaining_ = 1;
        wakeup_.notify_one();
    }
    drained_.wait(lock, [this] {
        return !processing_ && (events_.empty() || retryExhausted_) &&
               (viewState_ == EpochViewState::Ready || retryExhausted_);
    });
    return storageAvailable_ && events_.empty() && viewState_ == EpochViewState::Ready;
}

std::shared_ptr<const LearningSnapshot> LearningWriter::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

std::int64_t LearningWriter::resetRevision() const {
    std::lock_guard lock(mutex_);
    return resetRevision_;
}

EpochViewState LearningWriter::epochViewState() const {
    std::lock_guard lock(mutex_);
    return viewState_;
}

std::size_t LearningWriter::queuedEventsCount() const {
    std::lock_guard lock(mutex_);
    return events_.size();
}

void LearningWriter::pauseWorkerForTest(bool pause) {
    {
        std::lock_guard lock(mutex_);
        workerPaused_ = pause;
    }
    if (!pause) {
        resumeWorkerCv_.notify_all();
    }
}

void LearningWriter::pauseWorkerAfterReadForTest(bool pause) {
    {
        std::lock_guard lock(mutex_);
        workerPausedAfterRead_ = pause;
    }
    if (!pause) {
        resumeWorkerCv_.notify_all();
    }
}

bool LearningWriter::isWorkerWaitingAfterReadForTest() const {
    std::lock_guard lock(mutex_);
    return workerWaitingAfterRead_;
}

void LearningWriter::setOnReadyCallback(std::function<void()> callback) {
    std::function<void()> invokeImmediately;
    {
        std::lock_guard lock(mutex_);
        onReadyCallback_ = std::move(callback);
        if (viewState_ == EpochViewState::Ready && onReadyCallback_) {
            invokeImmediately = onReadyCallback_;
        }
    }
    if (invokeImmediately) {
        invokeImmediately();
    }
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
    std::function<void()> onReady;
    {
        std::lock_guard lock(mutex_);
        rebaseSnapshot(*loaded, true);
        storageAvailable_ = true;
        // A refreshed store starts a new bounded retry cycle. Otherwise a queue
        // exhausted before the clear would also leave new selections sleeping.
        retryExhausted_ = false;
        automaticRetryCyclesRemaining_ = 1;
        onReady = onReadyCallback_;
        wakeup_.notify_one();
    }
    if (onReady) {
        onReady();
    }
    return true;
}

bool LearningWriter::reloadIfChanged() {
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
        resetRevision_ = *revision;
        viewState_ = EpochViewState::Reloading;
        storageAvailable_ = true;
        retiredSnapshots_.push_back(std::move(snapshot_));
        snapshot_ = emptySnapshot();
        ++snapshotVersion_;
        std::erase_if(events_, [rev = *revision](const Event &event) {
            return event.resetRevision < rev;
        });
        retryExhausted_ = false;
        automaticRetryCyclesRemaining_ = 1;
        wakeup_.notify_one();
    }
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
        resetRevision_ = *revision;
        viewState_ = EpochViewState::Reloading;
        storageAvailable_ = true;
        retiredSnapshots_.push_back(std::move(snapshot_));
        snapshot_ = emptySnapshot();
        ++snapshotVersion_;
        std::erase_if(events_, [rev = *revision](const Event &event) {
            return event.resetRevision < rev;
        });
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
    case EventKind::PendingCommit:
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
    viewState_ = EpochViewState::Ready;
    snapshot_ = std::move(next);
    ++snapshotVersion_;
}

bool LearningWriter::persistBatch(const std::vector<Event> &events) {
    constexpr int kMaxAttemptsPerCycle = 3;
    std::vector<LearningEvent> storeEvents;
    storeEvents.reserve(events.size());
    for (const auto &event : events) {
        if (event.kind == EventKind::PendingCommit) {
            continue;
        }
        const auto kind = event.kind == EventKind::Selection
                              ? LearningEvent::Kind::Selection
                              : event.kind == EventKind::NegativeFeedback
                                    ? LearningEvent::Kind::NegativeFeedback
                                    : LearningEvent::Kind::Suppression;
        storeEvents.push_back({kind, event.phrase, event.pinyin,
                               event.contextBefore, event.contextAfter,
                               event.nowMs, event.resetRevision});
    }

    if (storeEvents.empty()) {
        return true;
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
        bool reloading = false;
        std::vector<Event> events;
        std::vector<std::shared_ptr<const LearningSnapshot>> toCleanup;
        {
            std::unique_lock lock(mutex_);
            if (!retiredSnapshots_.empty()) {
                toCleanup = std::move(retiredSnapshots_);
            }
            if (!stopping_ && retryExhausted_ &&
                (!events_.empty() || viewState_ == EpochViewState::Reloading) &&
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
                return stopping_ || (!events_.empty() && !retryExhausted_) ||
                       (viewState_ == EpochViewState::Reloading && !retryExhausted_);
            });
            if (stopping_ && (events_.empty() || retryExhausted_)) {
                return;
            }
            reloading = (viewState_ == EpochViewState::Reloading);
            if (!reloading) {
                events.reserve(events_.size());
                while (!events_.empty()) {
                    events.push_back(std::move(events_.front()));
                    events_.pop_front();
                }
                processing_ = true;
                processingEvents_ = events;
            }
        }

        if (reloading) {
            {
                std::unique_lock lock(mutex_);
                resumeWorkerCv_.wait(lock, [this] { return !workerPaused_ || stopping_; });
            }
            if (stopping_) {
                return;
            }
            std::optional<LearningStoreState> freshState;
            {
                std::lock_guard storeLock(storeMutex_);
                if (store_->open()) {
                    freshState = store_->readState();
                }
            }
            {
                std::unique_lock lock(mutex_);
                workerWaitingAfterRead_ = true;
                resumeWorkerCv_.wait(lock, [this] { return !workerPausedAfterRead_ || stopping_; });
                workerWaitingAfterRead_ = false;
            }
            if (stopping_) {
                return;
            }

            std::shared_ptr<LearningSnapshot> preparedSnapshot;
            if (freshState.has_value() && freshState->snapshot != nullptr) {
                preparedSnapshot = std::make_shared<LearningSnapshot>(*freshState->snapshot);
            }

            std::optional<std::int64_t> currentDiskRevision;
            {
                std::lock_guard storeLock(storeMutex_);
                if (store_->open()) {
                    currentDiskRevision = store_->resetRevision();
                }
            }

            std::function<void()> onReady;
            std::shared_ptr<const LearningSnapshot> retiredSnapshot;
            {
                std::lock_guard lock(mutex_);
                if (!retiredSnapshots_.empty()) {
                    toCleanup.insert(toCleanup.end(),
                                     std::make_move_iterator(retiredSnapshots_.begin()),
                                     std::make_move_iterator(retiredSnapshots_.end()));
                    retiredSnapshots_.clear();
                }
                if (!freshState.has_value() || preparedSnapshot == nullptr) {
                    storageAvailable_ = false;
                    retryExhausted_ = true;
                } else if (freshState->resetRevision < resetRevision_ ||
                           (currentDiskRevision.has_value() && *currentDiskRevision > freshState->resetRevision)) {
                    if (currentDiskRevision.has_value() && *currentDiskRevision > resetRevision_) {
                        resetRevision_ = *currentDiskRevision;
                    }
                    retryExhausted_ = false;
                    automaticRetryCyclesRemaining_ = 1;
                    wakeup_.notify_one();
                } else {
                    storageAvailable_ = true;
                    retryExhausted_ = false;
                    automaticRetryCyclesRemaining_ = 1;

                    auto freshSnapshot = std::move(preparedSnapshot);
                    std::deque<Event> validEvents;
                    for (auto &event : events_) {
                        if (event.resetRevision < freshState->resetRevision) {
                            continue;
                        }
                        if (event.kind == EventKind::PendingCommit) {
                            if (event.commitEvent.has_value()) {
                                const bool hasPositive =
                                    freshSnapshot->hasPositiveFrequency(event.commitEvent->text, event.pinyin);
                                const auto decision = decideCommitLearning(*event.commitEvent, hasPositive);
                                if (decision == CommitLearningDecision::Record) {
                                    event.kind = EventKind::Selection;
                                    event.phrase = event.commitEvent->text;
                                    freshSnapshot->recordSelection(event.phrase, event.pinyin,
                                                                   event.contextBefore, event.contextAfter,
                                                                   event.nowMs);
                                    validEvents.push_back(std::move(event));
                                }
                            }
                        } else {
                            applyEvent(*freshSnapshot, event);
                            validEvents.push_back(std::move(event));
                        }
                    }
                    events_ = std::move(validEvents);
                    while (events_.size() > kMaxQueuedEvents) {
                        events_.pop_front();
                    }
                    resetRevision_ = freshState->resetRevision;
                    retiredSnapshot = std::move(snapshot_);
                    snapshot_ = std::move(freshSnapshot);
                    ++snapshotVersion_;
                    viewState_ = EpochViewState::Ready;
                    onReady = onReadyCallback_;
                }
                drained_.notify_all();
            }
            if (onReady) {
                onReady();
            }
            continue;
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
            if (!retiredSnapshots_.empty()) {
                toCleanup.insert(toCleanup.end(),
                                 std::make_move_iterator(retiredSnapshots_.begin()),
                                 std::make_move_iterator(retiredSnapshots_.end()));
                retiredSnapshots_.clear();
            }
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
                           viewState_ == EpochViewState::Ready &&
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
