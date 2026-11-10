#pragma once
#include "modernime/core/settings.h"
#include <gtk/gtk.h>
#include <functional>
#include <cstddef>
#include <string>
#include <vector>

namespace modernime::settings {
struct PinCandidateResult {
    std::vector<core::CandidatePin> candidates;
    bool hasMore = false;
    std::string error;
};
// Blocking read-only query: call from Worker. Never select/commit or enable learning.
PinCandidateResult queryPinCandidates(const core::SettingsPaths &paths,
    const core::ModernIMESettings &settings, core::PinyinScheme scheme,
    const std::string &input, std::size_t limit);
GtkWidget *createCandidatePinDialog(GtkWindow *parent, const core::ModernIMESettings &settings,
    std::function<void(std::vector<core::CandidatePin>)> accept);
}
