#include "modernime/settings/pages/clipboard_page.h"

#include <type_traits>
#include <filesystem>
#include <functional>
#include <string>

int main() {
    using modernime::settings::ClipboardPage;
    using modernime::settings::SettingsPageId;

    static_assert(ClipboardPage::pageId == SettingsPageId::Clipboard);
    static_assert(!std::is_copy_constructible_v<ClipboardPage>);
    static_assert(std::is_constructible_v<
                  ClipboardPage, modernime::settings::SettingsWindowModel &,
                  std::filesystem::path, std::function<void()>,
                  std::function<void(std::string)>>);
    static_assert(std::is_same_v<decltype(&ClipboardPage::widget),
                                 modernime::settings::GtkWidget *
                                     (ClipboardPage::*)() const>);
    static_assert(std::is_same_v<decltype(&ClipboardPage::refresh),
                                 void (ClipboardPage::*)(bool)>);
    static_assert(std::is_same_v<decltype(&ClipboardPage::refreshSettings),
                                 void (ClipboardPage::*)()>);
    static_assert(std::is_same_v<decltype(&ClipboardPage::focusTarget),
                                 bool (ClipboardPage::*)(std::string_view)>);
    return 0;
}
