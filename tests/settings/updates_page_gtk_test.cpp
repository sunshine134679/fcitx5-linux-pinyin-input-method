#include "modernime/settings/pages/updates_page.h"
#include "modernime/settings/settings_widgets.h"
#include "modernime/settings/update_controller.h"

#include <gtk/gtk.h>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

// Intercept only the external browser boundary; the GTK button and page callbacks
// remain real, and the test never launches the user's browser.
std::string openedUri;
bool browserAvailable = true;
extern "C" gboolean gtk_show_uri_on_window(GtkWindow *, const gchar *uri, guint32, GError **error) {
    openedUri = uri;
    if (!browserAvailable) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "fixture browser unavailable");
        return FALSE;
    }
    return TRUE;
}

namespace {
int failures = 0;

void check(bool condition, const char *reason) {
    if (!condition) {
        std::cerr << "FAIL: " << reason << '\n';
        ++failures;
    }
}

void writeFile(const std::filesystem::path &path, std::string_view content) {
    std::ofstream out(path);
    out << content;
    out.close();
    assert(out.good());
}

GtkWidget *findTarget(GtkWidget *widget, std::string_view target) {
    auto *id = static_cast<const char *>(
        g_object_get_data(G_OBJECT(widget), "modernime-settings-target"));
    if (id && target == id) return widget;
    if (!GTK_IS_CONTAINER(widget)) return nullptr;
    auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
    GtkWidget *found = nullptr;
    for (auto *item = children; item && !found; item = item->next)
        found = findTarget(GTK_WIDGET(item->data), target);
    g_list_free(children);
    return found;
}

bool containsLabel(GtkWidget *widget, std::string_view text) {
    if (GTK_IS_LABEL(widget) && text == gtk_label_get_text(GTK_LABEL(widget)))
        return true;
    if (!GTK_IS_CONTAINER(widget)) return false;
    auto *children = gtk_container_get_children(GTK_CONTAINER(widget));
    bool found = false;
    for (auto *item = children; item && !found; item = item->next)
        found = containsLabel(GTK_WIDGET(item->data), text);
    g_list_free(children);
    return found;
}

template <typename Predicate> bool waitUntil(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        while (g_main_context_iteration(nullptr, FALSE)) {}
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

std::string visibleDetailsMessage(GtkWidget *page) {
    auto *toggle = findTarget(page, "updates-version-details");
    auto *details = findTarget(page, "updates-check-details-message");
    assert(GTK_IS_TOGGLE_BUTTON(toggle) && GTK_IS_LABEL(details));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), TRUE);
    assert(waitUntil([details] { return gtk_widget_get_mapped(details); }));
    return gtk_label_get_text(GTK_LABEL(details));
}
} // namespace

int main(int argc, char **argv) {
    if (!gtk_init_check(&argc, &argv)) return 77;
    using namespace modernime::settings;
    installSettingsStyles();
    std::string pattern = "/tmp/modernime-updates-page-XXXXXX";
    const auto *created = ::mkdtemp(pattern.data());
    assert(created);
    const std::filesystem::path root(created);
    std::filesystem::create_directory(root / "bin");
    std::filesystem::create_directories(root / "prefix/bin");
    writeFile(root / "channel", "channel=deb\n");
    // The external package query is the only double. Parsing, policy, GTask,
    // lifetime dispatch and the visible GTK consumer are production code.
    writeFile(root / "bin/apt-cache", R"sh(#!/bin/sh
case "$1 $2" in
  'policy modernime')
    printf 'query\n' >> "$UPDATE_TEST_ROOT/queries"
    if [ -f "$UPDATE_TEST_ROOT/hold" ]; then
      : > "$UPDATE_TEST_ROOT/started"
      count=0
      while [ -f "$UPDATE_TEST_ROOT/hold" ]; do
        /bin/sleep 0.01
        count=$((count + 1))
        [ "$count" -lt 400 ] || exit 99
      done
    fi
    [ ! -f "$UPDATE_TEST_ROOT/fail" ] || exit 100
    if [ -f "$UPDATE_TEST_ROOT/truncated" ]; then
      /usr/bin/head -c 1100000 /dev/zero
    else
      /bin/cat "$UPDATE_TEST_ROOT/policy"
    fi
    ;;
  'show modernime') /bin/cat "$UPDATE_TEST_ROOT/show" ;;
  *) exit 42 ;;
esac
)sh");
    assert(::chmod((root / "bin/apt-cache").c_str(), 0755) == 0);
    const Environment environment{
        {"PATH", (root / "bin").string()},
        {"UPDATE_TEST_ROOT", root.string()},
        {"MODERNIME_PREFIX", (root / "prefix").string()},
        {"XDG_CACHE_HOME", (root / "cache").string()},
        {"MODERNIME_CURRENT_VERSION", "0.1.23"},
        {"MODERNIME_INSTALL_CHANNEL_FILE", (root / "channel").string()},
        {"MODERNIME_UPDATE_METADATA_FILE", (root / "absent").string()},
        {"MODERNIME_PACKAGE_MANAGER_LOCK_FILE", (root / "lock").string()},
        {"MODERNIME_PACKAGE_OP_TIMEOUT_MS", "80"},
    };
    const auto policy = [&root](std::string_view version) {
        writeFile(root / "policy", "modernime:\n  Installed: 0.1.23\n  Candidate: " +
            std::string(version) + "\n  Version table:\n     " + std::string(version) +
            " 500\n        500 https://example.invalid/repo stable/main amd64 Packages\n");
        writeFile(root / "show", "Description: Fixture package description\n");
    };
    auto *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_style_context_add_class(gtk_widget_get_style_context(window), "modernime-settings");
    gtk_window_set_default_size(GTK_WINDOW(window), 560, 600);
    {
        std::string notification;
        bool allowRestart = false;
        bool restartRequested = false;
        UpdatesPage page(environment, [&notification](std::string text) { notification = text; },
                         [&allowRestart] { return allowRestart; },
                         [&restartRequested] { restartRequested = true; });
        auto *pageScroll = createScrollablePage(page.widget());
        gtk_container_add(GTK_CONTAINER(window), pageScroll);
        gtk_widget_show_all(window);
        auto *status = findTarget(page.widget(), "updates-status");
        auto *hint = findTarget(page.widget(), "updates-status-hint");
        auto *button = findTarget(page.widget(), "updates-check");
        auto *copy = findTarget(page.widget(), "updates-copy-command");
        auto *recheck = findTarget(page.widget(), "updates-recheck");
        auto *notesToggle = findTarget(page.widget(), "updates-release-details");
        auto *notes = findTarget(page.widget(), "updates-release-notes");
        assert(status && hint && button && copy);
        // Window activation is asynchronous. Search focus must be exercised
        // after the fixture owns keyboard focus, as in the real client.
        gtk_window_present(GTK_WINDOW(window));
        gtk_widget_grab_focus(button);
        assert(waitUntil([button] { return gtk_widget_has_focus(button); }));
        const auto message = [status] { return std::string(gtk_label_get_text(GTK_LABEL(status))); };
        const auto nextStep = [hint] { return std::string(gtk_label_get_text(GTK_LABEL(hint))); };
        const auto detailsMessage = [&page] { return visibleDetailsMessage(page.widget()); };
        const auto query = [&] {
            page.refresh();
            assert(!gtk_widget_get_sensitive(button));
            assert(waitUntil([button] { return gtk_widget_get_sensitive(button); }));
        };
        check(containsLabel(page.widget(), "尚未检查"), "unqueried installation channel must not be fabricated as deb");
        check(!gtk_widget_get_sensitive(copy), "unqueried upgrade command must not be actionable");

        for (const char *version : {"0.1.23", "0.1.22"}) {
            policy(version);
            query();
            check(message().find("本地软件源缓存") != std::string::npos,
                  "cached no-update result must disclose its local-cache scope");
            check(detailsMessage().find("检查更新") != std::string::npos,
                  "cached result must direct users to the integrated check action");
            check(containsLabel(page.widget(), version), "candidate must retain repository version even if older than installed");
        }

        policy("0.1.24");
        writeFile(root / "hold", "hold");
        page.refresh();
        assert(waitUntil([&root] { return std::filesystem::exists(root / "started"); }));
        check(!gtk_widget_get_sensitive(copy), "in-flight refresh must disable copying a previous upgrade command");
        check(!containsLabel(page.widget(), "0.1.22"), "in-flight refresh must not present the old candidate as current");
        check(message().find("正在") != std::string::npos, "in-flight query must remain visible");
        assert(page.focusTarget("updates-copy-command"));
        std::filesystem::remove(root / "hold");
        assert(waitUntil([button] { return gtk_widget_get_sensitive(button); }));
        check(gtk_window_get_focus(GTK_WINDOW(window)) == copy,
              "searching copy during a query must focus it once the command is ready");
        check(message().find("0.1.24") != std::string::npos && containsLabel(page.widget(), "0.1.24"),
              "newer candidate must reach both summary and version details");
        check(std::string_view(gtk_button_get_label(GTK_BUTTON(button))) == "下载更新",
              "available update must turn the primary action into download");
        check(!containsLabel(page.widget(), "Fixture package description"),
              "package descriptions must never be presented as release notes");
        check(gtk_widget_get_sensitive(copy), "completed query must restore copy action");
        gtk_button_clicked(GTK_BUTTON(copy));
        gchar *copied = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
        check(copied && std::string_view(copied) == "sudo apt update && sudo apt install --only-upgrade modernime",
              "copy must deliver the actual package-manager upgrade command");
        g_free(copied);
        check(!notification.empty(), "copy must provide completion feedback");

        std::filesystem::remove(root / "started");
        writeFile(root / "hold", "hold");
        page.refresh();
        assert(waitUntil([&root] { return std::filesystem::exists(root / "started"); }));
        assert(page.focusTarget("updates-copy-command"));
        assert(page.focusTarget("updates-source"));
        auto *source = findTarget(page.widget(), "updates-source-jump");
        assert(gtk_window_get_focus(GTK_WINDOW(window)) == source);
        std::filesystem::remove(root / "hold");
        assert(waitUntil([button] { return gtk_widget_get_sensitive(button); }));
        check(gtk_window_get_focus(GTK_WINDOW(window)) == source,
              "query completion must not override a newer focus target");

        writeFile(root / "show", "");
        query();
        check(containsLabel(page.widget(), "展开后加载官方更新说明。"),
              "local package metadata must leave official release notes explicitly unloaded");

        writeFile(root / "fail", "fail");
        query();
        const auto failureDetails = detailsMessage();
        check(failureDetails.find("软件源") != std::string::npos && failureDetails.find("缓存") != std::string::npos,
              "query failure must retain repository/cache causes instead of blaming only the network");
        check((message() + nextStep() + failureDetails).find("运行正常") == std::string::npos,
              "update query failure cannot certify local input method or data health");
        check(!containsLabel(page.widget(), "0.1.24"), "failed retry must clear previous candidate");
        std::filesystem::remove(root / "fail");
        writeFile(root / "truncated", "large");
        query();
        check(detailsMessage().find("截断") != std::string::npos, "truncated query must preserve its distinct diagnostic");
        std::filesystem::remove(root / "truncated");
        query();
        check(containsLabel(page.widget(), "0.1.24"), "successful retry must replace failure state");

        // Explicit empty channel is returned by the channel parser without
        // falling back to an installation on the machine running this test.
        writeFile(root / "channel", "channel=\n");
        query();
        check(containsLabel(page.widget(), "未识别"), "an available APT tool does not prove a deb installation");
        writeFile(root / "channel", "channel=script\n");
        query();
        gtk_button_clicked(GTK_BUTTON(copy));
        copied = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
        check(copied && std::string_view(copied) ==
                  "modernime-migrate-channel --to-channel deb --package-cmd \"sudo apt update && sudo apt install -y modernime\"",
              "script installation must retain the explicit channel migration command");
        g_free(copied);

        writeFile(root / "channel", "channel=rpm\n");
        writeFile(root / "bin/dnf", R"sh(#!/bin/sh
[ "$*" = '--cacheonly --setopt=*.repo_gpgcheck=0 -q info modernime' ] || exit 42
printf 'Available Packages\nName : modernime\nVersion : 0.1.22\nRelease : 1\nRepository : modernime-test\nSummary : RPM fixture\n'
)sh");
        assert(::chmod((root / "bin/dnf").c_str(), 0755) == 0);
        query();
        check(message().find("本地软件源缓存") != std::string::npos &&
              detailsMessage().find("检查更新") != std::string::npos,
              "RPM cached result must point to the integrated check action");
        check(containsLabel(page.widget(), "0.1.22-1"), "RPM candidate must not be substituted with installed version");
        gtk_button_clicked(GTK_BUTTON(copy));
        copied = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
        check(copied && std::string_view(copied) == "sudo dnf makecache --refresh && sudo dnf upgrade modernime",
              "RPM copy action must not fall back to an APT command");
        g_free(copied);
        writeFile(root / "channel", "channel=deb\n");

        // Real HTTPS request construction and release parsing execute in Python;
        // only the network transport is replaced. No host authentication/updates.
        writeFile(root / "bin/python3", R"py(#!/usr/bin/python3
import io, json, os, pathlib, sys, time, urllib.error, urllib.request
if sys.argv[1] != '-c':
    os.execv('/usr/bin/python3', ['/usr/bin/python3', *sys.argv[1:]])
root = pathlib.Path(os.environ['UPDATE_TEST_ROOT'])
def urlopen(request, timeout):
    if request.full_url.startswith('https://raw.githubusercontent.com/sunshine134679/fcitx5-linux-pinyin-input-method/'):
        assert timeout <= 6
        with (root / 'notes-queries').open('a') as log:
            log.write(request.full_url + '\n')
        if (root / 'notes-fail').exists():
            raise urllib.error.URLError('fixture notes unavailable')
        return io.BytesIO((root / 'release-notes.md').read_bytes())
    if request.full_url == 'https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method/releases/latest':
        assert request.get_method() == 'HEAD'
        response = io.BytesIO()
        response.geturl = lambda: 'https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method/releases/tag/' + json.loads((root / 'release.json').read_text())['tag_name']
        return response
    assert request.full_url == 'https://api.github.com/repos/sunshine134679/fcitx5-linux-pinyin-input-method/releases/latest'
    assert timeout <= 10
    with (root / 'online-queries').open('a') as log:
        log.write('query\n')
    (root / 'online-started').touch()
    deadline = time.monotonic() + 4
    while (root / 'online-hold').exists() and time.monotonic() < deadline:
        time.sleep(0.01)
    if (root / 'online-fail').exists():
        raise urllib.error.URLError('fixture network unavailable')
    if (root / 'rate-limited').exists():
        raise urllib.error.HTTPError(request.full_url, 403, 'rate limit exceeded', {}, None)
    return io.BytesIO((root / 'release.json').read_bytes())
urllib.request.urlopen = urlopen
exec(sys.argv[2])
)py");
        writeFile(root / "bin/pkexec", "#!/bin/sh\n: > \"$UPDATE_TEST_ROOT/authorization-forbidden\"\nexit 99\n");
        assert(::chmod((root / "bin/python3").c_str(), 0755) == 0);
        assert(::chmod((root / "bin/pkexec").c_str(), 0755) == 0);
        const auto release = [&root](std::string_view version) {
            writeFile(root / "release.json", "{\"tag_name\":\"v" + std::string(version) +
                "\",\"draft\":false,\"prerelease\":false,\"body\":\"正式发布说明\"}");
        };
        policy("0.1.22");
        release("0.1.25");
        writeFile(root / "online-hold", "hold");
        std::filesystem::remove(root / "started");
        writeFile(root / "hold", "hold");
        page.refresh();
        assert(waitUntil([&root] { return std::filesystem::exists(root / "started"); }));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(notesToggle), TRUE);
        std::filesystem::remove(root / "hold");
        assert(waitUntil([&root] { return std::filesystem::exists(root / "online-started"); }));
        auto *notesState = findTarget(page.widget(), "updates-release-state");
        auto *loadingScroll = findTarget(page.widget(), "updates-release-scroll");
        check(GTK_IS_LABEL(notesState) && waitUntil([&] { return gtk_widget_get_mapped(notesState); }) &&
                  std::string_view(gtk_label_get_text(GTK_LABEL(notesState))).find("正在加载") != std::string_view::npos,
              "opening release notes must expose a visible loading state outside the document viewport");
        check(!gtk_widget_get_visible(loadingScroll),
              "the empty document viewport must remain hidden until release notes are available");
        check(message().find("查询官方") != std::string::npos &&
                  !gtk_widget_get_sensitive(button) && !gtk_widget_get_sensitive(recheck),
              "online check must expose progress and disable duplicate actions");
        check(!UpdateController::hasActivePackageOperation(),
              "read-only online check must not acquire a package transaction");
        std::filesystem::remove(root / "online-hold");
        assert(waitUntil([button] { return gtk_widget_get_sensitive(button); }));
        check(message() == "可更新至 0.1.25" &&
                  std::string_view(gtk_button_get_label(GTK_BUTTON(button))) == "下载更新",
              "online release must replace cached version and enable download");
        check(containsLabel(page.widget(), "正式发布说明"),
              "online release notes must reach the page");
        check(notesState && !gtk_widget_get_mapped(notesState) && gtk_widget_get_mapped(loadingScroll),
              "loaded notes must replace the loading state with the visible document");
        check(waitUntil([&] {
                  const auto height = gtk_widget_get_allocated_height(loadingScroll);
                  return height > 1 && height < 120;
              }),
              "a short release note must fit its content without an empty fixed-height panel");
        writeFile(root / "online-fail", "fail");
        gtk_button_clicked(GTK_BUTTON(recheck));
        assert(waitUntil([button] { return gtk_widget_get_sensitive(button); }));
        check(message() == "暂时无法确认更新" &&
                  detailsMessage().find("fixture network unavailable") != std::string::npos &&
                  std::string_view(gtk_button_get_label(GTK_BUTTON(button))) == "重新检查",
              "network failure must clear the previous download action and preserve its cause");
        auto *notesRetry = findTarget(page.widget(), "updates-release-retry");
        check(containsLabel(page.widget(), "正式发布说明") && notesRetry &&
                  gtk_widget_get_mapped(notesRetry),
              "failed refresh must retain loaded release notes and provide a retry action");
        std::filesystem::remove(root / "online-fail");
        release("0.1.23");
        writeFile(root / "release.json", R"json({"tag_name":"v0.1.23","draft":false,"prerelease":false,"body":"# ModernIME v0.1.23\n\n## 输入辅助\n\n- **快捷短语**：输入 `foo_bar`，保留 <原文> & 符号。\n- 访问[官方说明](https://example.invalid/notes)。\n\n```text\n<code> & data\n```"})json");
        gtk_button_clicked(GTK_BUTTON(notesRetry));
        assert(waitUntil([button] { return gtk_widget_get_sensitive(button); }));
        check(message() == "未发现新版本" && nextStep().find("官方") != std::string::npos,
              "online no-update result must not ask users to refresh repositories");
        check(detailsMessage().find("sudo apt update") == std::string::npos,
              "online result must remove obsolete privileged-refresh instructions");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(notesToggle), TRUE);
        check(containsLabel(page.widget(),
                  "ModernIME v0.1.23\n\n输入辅助\n\n• 快捷短语：输入 foo_bar，保留 <原文> & 符号。\n• 访问官方说明。\n\n<code> & data"),
              "release notes must render Markdown headings, lists, emphasis, code and links as readable text");
        const std::string markup = gtk_label_get_label(GTK_LABEL(notes));
        check(gtk_label_get_use_markup(GTK_LABEL(notes)) &&
                  markup.find("size=\"x-large\"") != std::string::npos &&
                  markup.find("<b>快捷短语</b>") != std::string::npos &&
                  markup.find("<tt>foo_bar</tt>") != std::string::npos &&
                  markup.find("<a href=\"https://example.invalid/notes\"") != std::string::npos,
              "native release notes must retain heading, emphasis, code and clickable link formatting");
        gtk_widget_grab_focus(notes);
        check(gtk_window_get_focus(GTK_WINDOW(window)) == notes,
              "release notes must allow keyboard focus and text selection");
        auto *accessibleNotes = gtk_widget_get_accessible(notes);
        check(ATK_IS_HYPERTEXT(accessibleNotes) &&
                  atk_hypertext_get_n_links(ATK_HYPERTEXT(accessibleNotes)) == 1,
              "release notes must expose a real native hyperlink to keyboard and accessibility users");
        if (ATK_IS_HYPERTEXT(accessibleNotes)) {
            auto *link = atk_hypertext_get_link(ATK_HYPERTEXT(accessibleNotes), 0);
            auto *uri = link ? atk_hyperlink_get_uri(link, 0) : nullptr;
            check(uri && std::string_view(uri) == "https://example.invalid/notes",
                  "native release note hyperlinks must retain their destination");
            g_free(uri);
        }
        query();
        check(std::string_view(gtk_label_get_label(GTK_LABEL(notes))) == markup,
              "returning to the page must not replace loaded official notes with package metadata");

        writeFile(root / "channel", "channel=rpm\n");
        release("0.1.26");
        std::string longNotes = "# ModernIME v0.1.26\n\n";
        for (int line = 0; line < 500; ++line)
            longNotes += "- 更新说明第 " + std::to_string(line) + " 项：完整正文在区域内滚动查看。\n";
        writeFile(root / "release-notes.md", longNotes);
        writeFile(root / "rate-limited", "403");
        gtk_button_clicked(GTK_BUTTON(button));
        assert(waitUntil([button] { return gtk_widget_get_sensitive(button); }));
        check(message() == "可更新至 0.1.26" && containsLabel(page.widget(), "Fedora / openEuler 软件包"),
              "online checks must preserve the RPM installation channel");
        const std::string longText = gtk_label_get_text(GTK_LABEL(notes));
        check(longText.find("第 499 项") != std::string::npos && longText.size() > 12000,
              "API rate limit must load the complete tagged Markdown instead of a placeholder or truncated text");
        auto *notesScroll = findTarget(page.widget(), "updates-release-scroll");
        check(GTK_IS_SCROLLED_WINDOW(notesScroll), "long release notes must have an internal scroll area");
        if (GTK_IS_SCROLLED_WINDOW(notesScroll)) {
            auto *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(notesScroll));
            check(waitUntil([&] { return gtk_adjustment_get_upper(adjustment) > gtk_adjustment_get_page_size(adjustment); }),
                  "long release notes must overflow inside the bounded area");
            check(gtk_widget_get_allocated_height(notesScroll) <= 340,
                  "expanding long notes must not make the page grow to the full document height");
            auto *revealer = gtk_widget_get_ancestor(notesScroll, GTK_TYPE_REVEALER);
            check(revealer && waitUntil([&] {
                      int x = 0, y = 0;
                      return gtk_widget_translate_coordinates(notesScroll, revealer, 0, 0, &x, &y) &&
                          y >= 0 && y + gtk_widget_get_allocated_height(notesScroll) <=
                              gtk_widget_get_allocated_height(revealer);
                  }),
                  "the outer page must scroll instead of letting the disclosure crop the document's beginning");
            auto *horizontal = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(notesScroll));
            check(gtk_adjustment_get_upper(horizontal) <= gtk_adjustment_get_page_size(horizontal) &&
                      pango_layout_get_width(gtk_label_get_layout(GTK_LABEL(notes))) / PANGO_SCALE <=
                          gtk_adjustment_get_page_size(horizontal),
                  "release notes must wrap inside the reading area in a narrow window");
            check(gtk_adjustment_get_value(adjustment) == gtk_adjustment_get_lower(adjustment),
                  "a newly loaded release must start at the complete heading, not a retained scroll offset");
            gboolean scrolled = FALSE;
            g_signal_emit_by_name(notesScroll, "scroll-child", GTK_SCROLL_PAGE_FORWARD, FALSE, &scrolled);
            check(scrolled && waitUntil([&] { return gtk_adjustment_get_value(adjustment) > 0; }),
                  "native page scrolling must move within the release notes");
        }
        writeFile(root / "notes-fail", "fail");
        gtk_button_clicked(GTK_BUTTON(recheck));
        assert(waitUntil([button] { return gtk_widget_get_sensitive(button); }));
        check(message() == "可更新至 0.1.26" &&
                  std::string_view(gtk_label_get_text(GTK_LABEL(notes))) == longText &&
                  gtk_widget_get_mapped(notesRetry),
              "failed Markdown fallback must retain loaded notes and offer retry while preserving the checked version");
        auto *notesLink = findTarget(page.widget(), "updates-release-link");
        gtk_button_clicked(GTK_BUTTON(notesLink));
        check(openedUri == "https://github.com/sunshine134679/fcitx5-linux-pinyin-input-method/releases/tag/v0.1.26",
              "the browser action must open this exact release when the notes request fails");
        openedUri.clear();
        check(!std::filesystem::exists(root / "authorization-forbidden"),
              "repeated online checks must never invoke pkexec or request a password");

        // Exercise the real native action worker, process ownership and GTK flow.
        // Only the external downloader/installer boundary uses a private fixture.
        writeFile(root / "prefix/bin/modernime-update", R"py(import os, pathlib, sys, time
root = pathlib.Path(os.environ['UPDATE_TEST_ROOT'])
action = sys.argv[1]
directory = pathlib.Path(sys.argv[sys.argv.index('--directory') + 1])
with (root / (action + '-calls')).open('a') as log:
    log.write('call\n')
(root / (action + '-started')).touch()
if action == 'restart':
    sys.exit(0)
(directory / 'progress.ini').write_text('[update]\nphase=' + action + '\nreceived=50\ntotal=100\n')
deadline = time.monotonic() + 4
while (root / (action + '-hold')).exists() and time.monotonic() < deadline:
    time.sleep(0.01)
if (root / (action + '-fail')).exists():
    print('fixture installation failed', file=sys.stderr)
    sys.exit(1)
(root / (action + '-completed')).touch()
)py");
        auto *progress = findTarget(page.widget(), "updates-download-progress");
        assert(GTK_IS_PROGRESS_BAR(progress));
        writeFile(root / "channel", "channel=unknown\n");
        gtk_button_clicked(GTK_BUTTON(button));
        assert(waitUntil([button] { return gtk_widget_get_sensitive(button); }));
        auto *errorSummary = findTarget(page.widget(), "updates-error-summary");
        check(GTK_IS_LABEL(errorSummary) && gtk_widget_get_mapped(errorSummary) &&
                  std::string_view(gtk_label_get_text(GTK_LABEL(errorSummary))).find("安装记录") != std::string_view::npos,
              "missing installation metadata must explain the cause directly in the update card");
        check(notification.find("usage:") == std::string::npos && notification.find('\n') == std::string::npos &&
                  notification.size() < 100 && !std::filesystem::exists(root / "download-calls"),
              "unknown channel must stop before invoking the helper and show only a compact notice");
        writeFile(root / "channel", "channel=rpm\n");
        const auto onlineQuerySize = std::filesystem::file_size(root / "online-queries");
        writeFile(root / "download-hold", "hold");
        gtk_button_clicked(GTK_BUTTON(button));
        assert(waitUntil([progress] { return gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(progress)) == 0.5; }));
        check(!gtk_widget_get_visible(errorSummary), "retry must clear the previous inline error");
        check(gtk_widget_get_visible(progress) && !gtk_widget_get_sensitive(button),
              "native download must expose progress and disable duplicate actions");
        check(openedUri.empty(), "native download must never open a browser");
        check(!UpdateController::hasActivePackageOperation(), "download must not begin installation");
        std::filesystem::remove(root / "download-hold");
        assert(waitUntil([button] { return gtk_widget_get_sensitive(button); }));
        check(std::string_view(gtk_button_get_label(GTK_BUTTON(button))) == "重启并更新",
              "verified download must offer restart and update");
        check(!std::filesystem::exists(root / "install-started"), "download alone must not install");
        check(std::filesystem::file_size(root / "online-queries") == onlineQuerySize,
              "download must use the checked version without checking again");
        gtk_button_clicked(GTK_BUTTON(button));
        check(!std::filesystem::exists(root / "install-started"),
              "cancelling unsaved settings handling must cancel installation");
        allowRestart = true;
        writeFile(root / "install-fail", "fail");
        gtk_button_clicked(GTK_BUTTON(button));
        assert(waitUntil([button] { return gtk_widget_get_sensitive(button); }));
        check(detailsMessage().find("fixture installation failed") != std::string::npos && !restartRequested,
              "installation failure must retain its diagnostic in details and never restart");
        check(std::string_view(gtk_button_get_label(GTK_BUTTON(button))) == "重试安装",
              "failed installation must retain the verified download for retry");
        std::filesystem::remove(root / "install-fail");
        writeFile(root / "install-hold", "hold");
        gtk_button_clicked(GTK_BUTTON(button));
        assert(waitUntil([] { return UpdateController::currentPackageOperationPhase() == PackageOperationPhase::Continuing; }));
        check(!gtk_widget_get_sensitive(button), "background installation must retain its operation and keep UI responsive");
        std::filesystem::remove(root / "install-hold");
        assert(waitUntil([&restartRequested] { return restartRequested; }));
        assert(waitUntil([&root] { return std::filesystem::exists(root / "restart-started"); }));
        check(!UpdateController::hasActivePackageOperation(), "completed installation must release its transaction");
        check(std::filesystem::file_size(root / "download-calls") == 5,
              "installation retry must reuse the verified download");
        check(!std::filesystem::exists(root / "authorization-forbidden"),
              "download and script installation must not request administrator authorization");
        writeFile(root / "channel", "channel=deb\n");
        gtk_widget_destroy(window);
    }
    // Lock state is injected only through the supported scoped Environment.
    {
        auto lockedEnvironment = environment;
        lockedEnvironment.emplace_back("MODERNIME_MOCK_PACKAGE_MANAGER_LOCKED", "1");
        auto *lockedWindow = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        UpdatesPage page(lockedEnvironment, [](std::string) {});
        g_object_ref_sink(page.widget());
        gtk_container_add(GTK_CONTAINER(lockedWindow), page.widget());
        gtk_widget_show_all(lockedWindow);
        auto *button = findTarget(page.widget(), "updates-check");
        page.refresh();
        assert(waitUntil([button] { return gtk_widget_get_sensitive(button); }));
        auto *status = findTarget(page.widget(), "updates-status");
        const std::string message = gtk_label_get_text(GTK_LABEL(status));
        const auto details = visibleDetailsMessage(page.widget());
        check(message.find("包管理器") != std::string::npos && details.find("稍后") != std::string::npos,
              "package lock must take priority over available-update feedback");
        gtk_widget_destroy(lockedWindow);
        g_object_unref(page.widget());
    }
    std::filesystem::remove_all(root);
    std::cout << "update page GTK failures: " << failures << '\n';
    return failures == 0 ? 0 : 1;
}
