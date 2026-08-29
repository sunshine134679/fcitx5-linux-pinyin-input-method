#include "modernime/settings/pages/dictionary_page.h"

#include <type_traits>

int main() {
    using modernime::settings::DictionaryPage;
    using modernime::settings::SettingsPageId;

    static_assert(DictionaryPage::pageId == SettingsPageId::Dictionary);
    static_assert(!std::is_copy_constructible_v<DictionaryPage>);
    return 0;
}
