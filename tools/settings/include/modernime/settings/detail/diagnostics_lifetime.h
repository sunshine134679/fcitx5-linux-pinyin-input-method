#pragma once

#include <functional>
#include <utility>

namespace modernime::settings::detail {

template <typename Owner>
class DiagnosticsLifetime final {
public:
    explicit DiagnosticsLifetime(Owner *owner) : owner_(owner) {}

    DiagnosticsLifetime(const DiagnosticsLifetime &) = delete;
    DiagnosticsLifetime &operator=(const DiagnosticsLifetime &) = delete;

    void deactivate() { owner_ = nullptr; }

    template <typename Callback>
    bool withOwner(Callback &&callback) {
        if (owner_ == nullptr) {
            return false;
        }
        std::invoke(std::forward<Callback>(callback), *owner_);
        return true;
    }

private:
    Owner *owner_;
};

} // namespace modernime::settings::detail
