#pragma once

#include "modernime/settings/overview_model.h"

#include <functional>
#include <memory>

struct _GtkWidget;

namespace modernime::settings {

using GtkWidget = ::_GtkWidget;

class OverviewPage final {
public:
    static constexpr auto pageId = SettingsPageId::Overview;

    explicit OverviewPage(
        std::function<void(SettingsPageId)> navigate,
        std::function<void(OverviewSetupAction)> setup = {},
        std::function<void()> refresh = {});
    ~OverviewPage();

    OverviewPage(const OverviewPage &) = delete;
    OverviewPage &operator=(const OverviewPage &) = delete;

    GtkWidget *widget() const;
    void setSnapshot(const OverviewSnapshot &snapshot);
    void setSetupBusy(bool busy);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::settings
