#include "modernime/settings/pages/updates_page.h"
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
    std::string pattern = "/tmp/modernime-updates-page-XXXXXX";
    const auto *created = ::mkdtemp(pattern.data());
    assert(created);
    const std::filesystem::path root(created);
    std::filesystem::create_directory(root / "bin");
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
    {
        std::string notification;
        UpdatesPage page(environment, [&notification](std::string text) { notification = text; });
        gtk_container_add(GTK_CONTAINER(window), page.widget());
        gtk_widget_show_all(window);
        auto *status = findTarget(page.widget(), "updates-status");
        auto *hint = findTarget(page.widget(), "updates-status-hint");
        auto *button = findTarget(page.widget(), "updates-check");
        auto *copy = findTarget(page.widget(), "updates-copy-command");
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
        check(containsLabel(page.widget(), "Fixture package description"), "real package description must be retained");
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
        check(containsLabel(page.widget(), "暂无更新说明"), "missing release metadata must not invent features");

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

        // Only the external package tools are doubled. A button click must
        // refresh first, wait for its own transaction, then query the new cache.
        writeFile(root / "bin/pkexec", R"sh(#!/bin/sh
printf 'authorize\n' >> "$UPDATE_TEST_ROOT/authorizations"
[ ! -f "$UPDATE_TEST_ROOT/cancel" ] || exit 126
exec "$@"
)sh");
        writeFile(root / "bin/apt-get", R"sh(#!/bin/sh
[ "$*" = 'update --error-on=any' ] || exit 42
: > "$UPDATE_TEST_ROOT/refresh-started"
count=0
while [ -f "$UPDATE_TEST_ROOT/refresh-hold" ]; do
  /bin/sleep 0.01
  count=$((count + 1))
  [ "$count" -lt 400 ] || exit 99
done
if [ -f "$UPDATE_TEST_ROOT/refresh-fail" ]; then
  echo 'Failed to fetch repository metadata' >&2
  exit 100
fi
/bin/cp "$UPDATE_TEST_ROOT/refreshed-policy" "$UPDATE_TEST_ROOT/policy"
)sh");
        assert(::chmod((root / "bin/pkexec").c_str(), 0755) == 0);
        assert(::chmod((root / "bin/apt-get").c_str(), 0755) == 0);
        policy("0.1.25");
        std::filesystem::copy_file(root / "policy", root / "refreshed-policy");
        policy("0.1.22");
        query();
        check(!std::filesystem::exists(root / "authorizations"),
              "page entry must never request authorization or refresh repositories");
        writeFile(root / "refresh-hold", "hold");
        gtk_button_clicked(GTK_BUTTON(button));
        if (waitUntil([&] { return std::filesystem::exists(root / "refresh-started"); })) {
            check(message().find("刷新软件源") != std::string::npos &&
                      !gtk_widget_get_sensitive(button) && !gtk_widget_get_sensitive(copy),
                  "refresh must expose progress and disable duplicate actions");
            check(waitUntil([] { return UpdateController::currentPackageOperationPhase() ==
                                          PackageOperationPhase::Continuing; }),
                  "fixture must exercise a refresh that outlives the initial wait");
            check(!gtk_widget_get_sensitive(button), "continuing refresh must keep the same check pending");
        } else {
            check(false, "check button must start the package cache refresh");
        }
        std::filesystem::remove(root / "refresh-hold");
        assert(waitUntil([button] { return gtk_widget_get_sensitive(button); }));
        check(message().find("0.1.25") != std::string::npos,
              "one click must query the newly refreshed cache without a second action");
        check(UpdateController::unconsumedPackageOperationCount() == 0,
              "check must consume only its completed refresh result");

        policy("0.1.23");
        std::filesystem::copy_file(root / "policy", root / "refreshed-policy",
                                   std::filesystem::copy_options::overwrite_existing);
        gtk_button_clicked(GTK_BUTTON(button));
        assert(waitUntil([button] { return gtk_widget_get_sensitive(button); }));
        check(message() == "未发现新版本" && nextStep().find("已刷新") != std::string::npos,
              "successful online check must not ask the user to refresh again");
        check(detailsMessage().find("sudo apt update") == std::string::npos,
              "completed refresh must remove obsolete manual refresh instructions");

        const auto querySize = std::filesystem::file_size(root / "queries");
        writeFile(root / "refresh-fail", "fail");
        gtk_button_clicked(GTK_BUTTON(button));
        assert(waitUntil([button] { return gtk_widget_get_sensitive(button); }));
        check(message().find("未完成") != std::string::npos &&
                  detailsMessage().find("Failed to fetch") != std::string::npos,
              "failed refresh must expose the actual cause instead of reporting no updates");
        check(std::filesystem::file_size(root / "queries") == querySize &&
                  !gtk_widget_get_sensitive(copy),
              "failed refresh must not query stale metadata or expose an old upgrade action");
        std::filesystem::remove(root / "refresh-fail");
        writeFile(root / "cancel", "cancel");
        gtk_button_clicked(GTK_BUTTON(button));
        assert(waitUntil([button] { return gtk_widget_get_sensitive(button); }));
        check(detailsMessage().find("取消") != std::string::npos &&
                  std::filesystem::file_size(root / "queries") == querySize,
              "cancelled authorization must end the check without querying stale metadata");
        std::filesystem::remove(root / "cancel");

        writeFile(root / "channel", "channel=rpm\n");
        writeFile(root / "bin/dnf", R"sh(#!/bin/sh
case "$*" in
  'makecache --refresh --setopt=*.skip_if_unavailable=False')
    : > "$UPDATE_TEST_ROOT/dnf-refreshed" ;;
  '--cacheonly --setopt=*.repo_gpgcheck=0 -q info modernime')
    [ -f "$UPDATE_TEST_ROOT/dnf-refreshed" ] || exit 42
    printf 'Available Packages\nName : modernime\nVersion : 0.1.26\nRelease : 1\nRepository : modernime-test\nSummary : RPM fixture\n' ;;
  *) exit 42 ;;
esac
)sh");
        gtk_button_clicked(GTK_BUTTON(button));
        assert(waitUntil([button] { return gtk_widget_get_sensitive(button); }));
        check(message().find("0.1.26-1") != std::string::npos,
              "RPM one-click check must refresh with DNF even when APT is present");
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
