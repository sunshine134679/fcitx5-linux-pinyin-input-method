#include "modernime/settings/pages/learning_page.h"

#include <type_traits>

int main() {
    using modernime::settings::LearningPage;
    using modernime::settings::SettingsPageId;

    static_assert(LearningPage::pageId == SettingsPageId::Learning);
    static_assert(!std::is_copy_constructible_v<LearningPage>);
    return 0;
}
