#pragma once

#include "modernime/settings/page_registry.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

struct _GtkWidget;

namespace modernime::settings {

using GtkWidget = ::_GtkWidget;

class DictionaryPage final {
public:
    static constexpr auto pageId = SettingsPageId::Dictionary;

    DictionaryPage(std::filesystem::path dictionaryPath,
                   std::function<void(std::string)> notify);
    ~DictionaryPage();

    DictionaryPage(const DictionaryPage &) = delete;
    DictionaryPage &operator=(const DictionaryPage &) = delete;

    GtkWidget *widget() const;
    void refresh();
    bool focusTarget(std::string_view target);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::settings
