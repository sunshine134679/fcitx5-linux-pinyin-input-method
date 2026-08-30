#include "modernime/settings/pages/diagnostics_page.h"

#include <type_traits>

int main() {
    using modernime::settings::DiagnosticsPage;
    using modernime::settings::SettingsPageId;

    static_assert(DiagnosticsPage::pageId == SettingsPageId::Diagnostics);
    static_assert(!std::is_copy_constructible_v<DiagnosticsPage>);
}
