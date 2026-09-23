#include "modernime/settings/pages/clipboard_page.h"

#include <cassert>
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

    using modernime::settings::analyzeClipboardContent;
    using modernime::settings::countLines;
    using modernime::settings::countUtf8Characters;
    using modernime::settings::makeCollapsedPreview;

    // UTF-8 counting
    assert(countUtf8Characters("hello") == 5);
    assert(countUtf8Characters("你好世界") == 4);
    assert(countUtf8Characters("a你b好c") == 5);

    // Line counting
    assert(countLines("") == 0);
    assert(countLines("single line") == 1);
    assert(countLines("line 1\nline 2\nline 3") == 3);

    // Collapsed preview: short text remains intact
    assert(makeCollapsedPreview("短文本") == "短文本");

    // Collapsed preview: multi-line truncation (> 3 lines)
    const std::string multiLines = "line1\nline2\nline3\nline4\nline5";
    const auto preview = makeCollapsedPreview(multiLines, 3, 120);
    assert(preview.find("line1") != std::string::npos);
    assert(preview.find("line2") != std::string::npos);
    assert(preview.find("line3") != std::string::npos);
    assert(preview.find("line4") == std::string::npos);
    assert(preview.find("…") != std::string::npos);

    // Content analysis: URL
    const auto urlMeta = analyzeClipboardContent("https://github.com/fcitx/fcitx5");
    assert(urlMeta.typeBadge == "🔗 链接");
    assert(urlMeta.badgeClass == "modernime-badge-url");
    assert(!urlMeta.isLong);

    // Content analysis: Code
    const auto codeMeta =
        analyzeClipboardContent("#include <iostream>\nint main() {\n  return 0;\n}");
    assert(codeMeta.typeBadge.find("代码") != std::string::npos);
    assert(codeMeta.badgeClass == "modernime-badge-code");
    assert(codeMeta.isLong);

    // Content analysis: Makefile snippet
    const auto makefileMeta =
        analyzeClipboardContent(".PHONY: debug\nall: build\n\t$(MAKE) -C build");
    assert(makefileMeta.typeBadge.find("代码") != std::string::npos);
    assert(makefileMeta.badgeClass == "modernime-badge-code");
    assert(makefileMeta.isLong);

    // Content analysis: Plain multiline text
    const auto multiTextMeta =
        analyzeClipboardContent("第一行文字\n第二行文字\n第三行文字");
    assert(multiTextMeta.typeBadge.find("多行") != std::string::npos);
    assert(multiTextMeta.badgeClass == "modernime-badge-multiline");
    assert(multiTextMeta.isLong);

    // Content analysis: Plain short text
    const auto textMeta = analyzeClipboardContent("普通纯文本内容");
    assert(textMeta.typeBadge == "📝 文本");
    assert(textMeta.badgeClass == "modernime-badge-text");
    assert(!textMeta.isLong);

    return 0;
}
