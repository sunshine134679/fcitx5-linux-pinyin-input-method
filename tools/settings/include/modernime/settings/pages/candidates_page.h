#pragma once

#include "modernime/settings/page_registry.h"
#include "modernime/settings/settings_model.h"

#include <functional>
#include <memory>
#include <string_view>

struct _GtkWidget;

namespace modernime::settings {

using GtkWidget = ::_GtkWidget;

class CandidatesPage final {
public:
    static constexpr SettingsPageId pageId = SettingsPageId::Candidates;

    CandidatesPage(SettingsWindowModel &model, std::function<void()> changed);
    ~CandidatesPage();

    CandidatesPage(const CandidatesPage &) = delete;
    CandidatesPage &operator=(const CandidatesPage &) = delete;

    GtkWidget *widget() const;
    void refresh();
    bool focusTarget(std::string_view target);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::settings
