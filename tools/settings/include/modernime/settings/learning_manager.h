#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>

struct _GtkWidget;

namespace modernime::settings {

// Runtime learning operations are independent of the settings draft/save cycle.
class LearningManager final {
public:
    explicit LearningManager(std::function<void(std::string)> notify);
    ~LearningManager();
    LearningManager(const LearningManager &) = delete;
    LearningManager &operator=(const LearningManager &) = delete;

    _GtkWidget *widget() const;
    void refresh();
    bool focusTarget(std::string_view target);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::settings
