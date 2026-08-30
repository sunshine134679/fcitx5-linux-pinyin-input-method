#pragma once

#include "modernime/settings/page_registry.h"
#include "modernime/settings/runtime_controller.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

struct _GtkWidget;

namespace modernime::settings {

using GtkWidget = ::_GtkWidget;

class DiagnosticsPage final {
public:
    static constexpr auto pageId = SettingsPageId::Diagnostics;

    DiagnosticsPage(std::filesystem::path fcitx,
                    std::filesystem::path remote,
                    Environment environment,
                    std::function<void(std::string)> notify);
    ~DiagnosticsPage();

    DiagnosticsPage(const DiagnosticsPage &) = delete;
    DiagnosticsPage &operator=(const DiagnosticsPage &) = delete;

    GtkWidget *widget() const;
    void refresh();
    void reload();
    bool focusTarget(std::string_view target);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::settings
