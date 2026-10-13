#pragma once

#include "modernime/settings/page_registry.h"
#include "modernime/settings/runtime_environment.h"
#include "modernime/settings/update_controller.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

struct _GtkWidget;

namespace modernime::settings {

using GtkWidget = ::_GtkWidget;

class UpdatesPage final {
public:
    static constexpr SettingsPageId pageId = SettingsPageId::Updates;

    UpdatesPage(Environment environment,
                std::function<void(std::string)> notify);
    ~UpdatesPage();

    UpdatesPage(const UpdatesPage &) = delete;
    UpdatesPage &operator=(const UpdatesPage &) = delete;

    GtkWidget *widget() const;
    void refresh();
    bool focusTarget(std::string_view target);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::settings
