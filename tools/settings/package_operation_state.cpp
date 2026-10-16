#include "modernime/settings/package_operation_state.h"

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace modernime::settings {
namespace {

class PackageOperationStateManager final {
public:
    static PackageOperationStateManager &instance() {
        static auto *manager = new PackageOperationStateManager();
        return *manager;
    }

    std::optional<std::uint64_t> tryBegin(PackageOperationKind kind) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (currentPhase_ != PackageOperationPhase::Idle) {
            return std::nullopt;
        }
        if (unconsumedResults_.size() >= kMaxUnconsumedResults) {
            return std::nullopt;
        }
        const std::uint64_t id = ++nextOperationId_;
        activeOperationId_ = id;
        currentKind_ = kind;
        currentPhase_ = PackageOperationPhase::Starting;
        return id;
    }

    bool advance(std::uint64_t id,
                 PackageOperationPhase expected,
                 PackageOperationPhase next) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (activeOperationId_ != id) {
            return false;
        }
        if (currentPhase_ != expected) {
            return false;
        }
        bool valid = false;
        switch (expected) {
            case PackageOperationPhase::Starting:
                valid = (next == PackageOperationPhase::Running ||
                         next == PackageOperationPhase::Uncertain ||
                         next == PackageOperationPhase::Completed);
                break;
            case PackageOperationPhase::Running:
                valid = (next == PackageOperationPhase::Continuing ||
                         next == PackageOperationPhase::Uncertain ||
                         next == PackageOperationPhase::Completed);
                break;
            case PackageOperationPhase::Continuing:
                valid = (next == PackageOperationPhase::Uncertain ||
                         next == PackageOperationPhase::Completed);
                break;
            case PackageOperationPhase::Uncertain:
                valid = (next == PackageOperationPhase::Completed);
                break;
            default:
                valid = false;
                break;
        }
        if (!valid) {
            return false;
        }
        currentPhase_ = next;
        if (next == PackageOperationPhase::Completed) {
            activeOperationId_ = 0;
            currentPhase_ = PackageOperationPhase::Idle;
        }
        cv_.notify_all();
        return true;
    }

    bool finish(std::uint64_t id,
                const RuntimeResult &result,
                std::string stdoutText,
                std::string stderrText,
                bool recordUnconsumed) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (activeOperationId_ != id) {
            return false;
        }
        if (currentPhase_ == PackageOperationPhase::Idle) {
            return false;
        }

        if (recordUnconsumed && unconsumedResults_.size() < kMaxUnconsumedResults) {
            unconsumedResults_.push_back(CompletedPackageOperation{
                id,
                currentKind_,
                result,
                stdoutText,
                stderrText
            });
        }
        lastStdout_ = std::move(stdoutText);
        lastStderr_ = std::move(stderrText);
        activeOperationId_ = 0;
        currentPhase_ = PackageOperationPhase::Idle;
        cv_.notify_all();
        return true;
    }

    bool isActive() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return currentPhase_ == PackageOperationPhase::Starting ||
               currentPhase_ == PackageOperationPhase::Running ||
               currentPhase_ == PackageOperationPhase::Continuing ||
               currentPhase_ == PackageOperationPhase::Uncertain;
    }

    PackageOperationPhase phase() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return currentPhase_;
    }

    std::optional<std::uint64_t> activeId() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (currentPhase_ == PackageOperationPhase::Idle) {
            return std::nullopt;
        }
        return activeOperationId_;
    }

    std::size_t unconsumedCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return unconsumedResults_.size();
    }

    bool waitForActive(unsigned int timeoutMs) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(
            lock,
            std::chrono::milliseconds(timeoutMs),
            [this]() {
                return currentPhase_ == PackageOperationPhase::Idle;
            });
    }

    std::optional<RuntimeResult> takeCompleted(std::optional<std::uint64_t> id) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (id.has_value()) {
            for (auto it = unconsumedResults_.begin(); it != unconsumedResults_.end(); ++it) {
                if (it->operationId == *id) {
                    RuntimeResult res = it->result;
                    unconsumedResults_.erase(it);
                    return res;
                }
            }
            return std::nullopt;
        }
        if (unconsumedResults_.empty()) {
            return std::nullopt;
        }
        RuntimeResult res = unconsumedResults_.front().result;
        unconsumedResults_.pop_front();
        return res;
    }

    std::string lastStdout() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastStdout_;
    }

    std::string lastStderr() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastStderr_;
    }

    void resetForTesting() {
        std::lock_guard<std::mutex> lock(mutex_);
        activeOperationId_ = 0;
        currentPhase_ = PackageOperationPhase::Idle;
        unconsumedResults_.clear();
        lastStdout_.clear();
        lastStderr_.clear();
        cv_.notify_all();
    }

private:
    PackageOperationStateManager() = default;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::uint64_t nextOperationId_{0};
    std::uint64_t activeOperationId_{0};
    PackageOperationKind currentKind_{PackageOperationKind::RefreshCache};
    PackageOperationPhase currentPhase_{PackageOperationPhase::Idle};
    std::deque<CompletedPackageOperation> unconsumedResults_;
    std::string lastStdout_;
    std::string lastStderr_;
};

} // namespace

std::optional<std::uint64_t> tryBeginPackageOperation(PackageOperationKind kind) {
    return PackageOperationStateManager::instance().tryBegin(kind);
}

bool advancePackageOperation(std::uint64_t id,
                             PackageOperationPhase expected,
                             PackageOperationPhase next) {
    return PackageOperationStateManager::instance().advance(id, expected, next);
}

bool finishPackageOperation(std::uint64_t id,
                            const RuntimeResult &result,
                            std::string stdoutText,
                            std::string stderrText,
                            bool recordUnconsumed) {
    return PackageOperationStateManager::instance().finish(
        id, result, std::move(stdoutText), std::move(stderrText), recordUnconsumed);
}

bool hasActivePackageOperation() {
    return PackageOperationStateManager::instance().isActive();
}

PackageOperationPhase currentPackageOperationPhase() {
    return PackageOperationStateManager::instance().phase();
}

std::optional<std::uint64_t> currentPackageOperationId() {
    return PackageOperationStateManager::instance().activeId();
}

std::size_t unconsumedPackageOperationCount() {
    return PackageOperationStateManager::instance().unconsumedCount();
}

bool waitForActivePackageOperation(unsigned int timeoutMs) {
    return PackageOperationStateManager::instance().waitForActive(timeoutMs);
}

std::optional<RuntimeResult> takeCompletedPackageOperationResult(
    std::optional<std::uint64_t> id) {
    return PackageOperationStateManager::instance().takeCompleted(id);
}

std::string lastCompletedPackageOperationStdout() {
    return PackageOperationStateManager::instance().lastStdout();
}

void resetPackageOperationStateForTesting() {
    PackageOperationStateManager::instance().resetForTesting();
}

} // namespace modernime::settings
