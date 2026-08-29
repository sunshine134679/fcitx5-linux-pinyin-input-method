#include "modernime/settings/pages/clipboard_page.h"

#include <type_traits>

int main() {
    using modernime::settings::ClipboardPage;
    using modernime::settings::SettingsPageId;

    static_assert(ClipboardPage::pageId == SettingsPageId::Clipboard);
    static_assert(!std::is_copy_constructible_v<ClipboardPage>);
    return 0;
}
