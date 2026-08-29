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
        std::function<void(SettingsPageId)> navigate);
    ~OverviewPage();

    OverviewPage(const OverviewPage &) = delete;
    OverviewPage &operator=(const OverviewPage &) = delete;

    GtkWidget *widget() const;
    void setSnapshot(const OverviewSnapshot &snapshot);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::settings
