#include "modernime/ui/ui_addon.h"
#include "modernime/ui/candidate_bar_layout.h"
#include "modernime/core/settings.h"
#include "modernime/fcitx5/fcitx_engine.h"

#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx-utils/signals.h>
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

void assertTrue(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "UI addon test failed: " << message << std::endl;
        std::exit(EXIT_FAILURE);
    }
}

class TestInputContext final : public fcitx::InputContext {
public:
    explicit TestInputContext(fcitx::InputContextManager &manager)
        : InputContext(manager, "modernime-ui-test") {
        created();
    }
    ~TestInputContext() override { destroy(); }
    const char *frontend() const override { return "modernime-ui-test"; }
    void updatePreeditImpl() override {}
    std::vector<std::string> commits;
    void commitStringImpl(const std::string &text) override { commits.push_back(text); }
    void forwardKeyImpl(const fcitx::ForwardKeyEvent &) override {}
    void deleteSurroundingTextImpl(int, unsigned int) override {}
};

class TestCandidateWord final : public fcitx::CandidateWord {
public:
    TestCandidateWord(std::string text, int index, int *selectedStore)
        : text_(std::move(text)), index_(index), selectedStore_(selectedStore) {
        setText(fcitx::Text(text_));
    }
    void select(fcitx::InputContext *context) const override {
        context->commitString(text().toString());
        if (selectedStore_ != nullptr) {
            *selectedStore_ = index_;
        }
        if (afterSelect) afterSelect();
    }
    std::function<void()> afterSelect;
    const std::string &candidateText() const { return text_; }
    void changeText(std::string value) { setText(fcitx::Text(std::move(value))); }
private:
    std::string text_;
    int index_;
    int *selectedStore_;
};

void drainGtkEvents() {
    for (int i = 0; i < 128 && g_main_context_iteration(nullptr, FALSE); ++i) {}
}

void paint(modernime::ui::ModernIMEUserInterface &ui) {
    auto *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1200, 400);
    auto *cr = cairo_create(surface);
    ui.draw(cr);
    assertTrue(cairo_status(cr) == CAIRO_STATUS_SUCCESS, "production draw succeeds");
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    drainGtkEvents();
}

std::vector<unsigned char> capture(modernime::ui::ModernIMEUserInterface &ui) {
    auto *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1200, 400);
    auto *cr = cairo_create(surface);
    ui.draw(cr);
    assertTrue(cairo_status(cr) == CAIRO_STATUS_SUCCESS, "capture production draw");
    cairo_surface_flush(surface);
    const auto *data = cairo_image_surface_get_data(surface);
    std::vector<unsigned char> pixels(
        data, data + cairo_image_surface_get_stride(surface) * 400);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    drainGtkEvents();
    return pixels;
}

GtkWidget *candidateArea() {
    GList *windows = gtk_window_list_toplevels();
    GtkWidget *area = nullptr;
    for (auto *it = windows; it; it = it->next) {
        auto *child = gtk_bin_get_child(GTK_BIN(it->data));
        if (child && GTK_IS_DRAWING_AREA(child)) {
            assertTrue(area == nullptr, "one isolated candidate drawing area");
            area = child;
        }
    }
    g_list_free(windows);
    assertTrue(area != nullptr, "production candidate drawing area exists");
    return area;
}

GdkEvent *buttonEvent(GtkWidget *area) {
    auto *event = gdk_event_new(GDK_BUTTON_PRESS);
    event->button.window = GDK_WINDOW(g_object_ref(gtk_widget_get_window(area)));
    event->button.button = 1;
    event->button.x = 30;
    event->button.y = 20;
    gdk_event_set_device(event, gdk_seat_get_pointer(
        gdk_display_get_default_seat(gtk_widget_get_display(area))));
    return event;
}

void click() {
    auto *area = candidateArea();
    auto *event = buttonEvent(area);
    gtk_widget_event(area, event);
    gdk_event_free(event);
}

std::unique_ptr<fcitx::CommonCandidateList> words(int *selected) {
    auto list = std::make_unique<fcitx::CommonCandidateList>();
    list->setPageSize(5);
    for (int i = 0; i < 10; ++i) {
        list->append(std::make_unique<TestCandidateWord>(
            i < 5 ? "旧词" : "新词", i, selected));
    }
    return list;
}

class PrefixProvider final : public modernime::core::CandidateProvider {
public:
    std::size_t consumed = 5;
    std::vector<std::string> texts = {"你好"};
    bool append(std::string_view input) override {
        page_.rawInput.append(input);
        page_.preedit = page_.rawInput;
        page_.items.clear();
        for (const auto &text : texts) {
            modernime::core::CandidateItem item;
            item.text = text;
            item.fullPinyin = "ni'hao";
            item.consumedInputBytes = consumed;
            page_.items.push_back(std::move(item));
        }
        ++page_.generation;
        return true;
    }
    bool select(std::size_t index) override {
        if (index >= page_.items.size()) return false;
        page_.rawInput.erase(0, consumed);
        page_.preedit = page_.rawInput;
        page_.items.clear();
        ++page_.generation;
        return true;
    }
    bool eraseLast() override { return false; }
    void reset() override { page_.clear(); }
    const modernime::core::CandidatePage &page() const override { return page_; }
private:
    modernime::core::CandidatePage page_;
};

void testOutputProjection(fcitx::Instance &instance) {
    TestInputContext context(instance.inputContextManager());
    context.focusIn();
    bool converted = false;
    int filteredCandidates = 0;
    const auto project = [](std::string text) {
        if (text.rfind("软件", 0) == 0) {
            text.replace(0, std::string("软件").size(), "軟體顯示轉換測試詞");
        }
        return text;
    };
    // Exercise the real Instance output-filter signal without loading a
    // converter or reading the user's Fcitx configuration.
    fcitx::ScopedConnection outputFilter = instance.connect<fcitx::Instance::OutputFilter>(
        [&](fcitx::InputContext *ic, fcitx::Text &text) {
            assertTrue(ic == &context, "output filter receives the owning input context");
            if (converted) {
                const auto original = text.toString();
                const auto projected = project(original);
                if (projected != original) ++filteredCandidates;
                text = fcitx::Text(projected);
            }
        });
    assertTrue(outputFilter.connected(), "real Instance output filter is connected");

    modernime::ui::ModernIMEUserInterface ui(&instance);
    PrefixProvider provider;
    provider.texts.clear();
    for (int i = 0; i < 9; ++i) provider.texts.push_back("软件" + std::to_string(i));
    modernime::fcitx5::FcitxEngineHost host(context);
    modernime::fcitx5::ModernIMEController controller(host, &provider);
    host.setController(controller);
    for (const char c : std::string("nihaoma"))
        controller.handle({modernime::fcitx5::KeyKind::Character, c});
    ui.update(fcitx::UserInterfaceComponent::InputPanel, &context);
    paint(ui);
    auto list = std::dynamic_pointer_cast<modernime::fcitx5::FcitxCandidateList>(
        context.inputPanel().candidateList());
    assertTrue(list != nullptr, "projection uses real controller candidate words");
    const auto rawPageSize = list->size();
    const auto rawPixels = capture(ui);
    const auto &firstWord = static_cast<const modernime::fcitx5::FcitxCandidateWord &>(
        list->candidate(0));
    const auto originalRequest = firstWord.request();

    converted = true;
    assertTrue(!ui.handleButtonPress(120, 20) && context.commits.empty(),
               "changed output filter revokes the previously painted geometry");
    ui.update(fcitx::UserInterfaceComponent::InputPanel, &context);
    paint(ui);
    const auto projectedPixels = capture(ui);
    assertTrue(filteredCandidates > 0 && list->size() < rawPageSize,
               "physical pagination measures the actual filtered text width");
    assertTrue(firstWord.text().toString() == "软件0" &&
                   firstWord.request().expected == originalRequest.expected &&
                   firstWord.request().displayGeneration == originalRequest.displayGeneration,
               "display projection preserves original text, expected identity and generation");
    std::vector<std::string> displayed;
    for (int i = 0; i < list->size(); ++i)
        displayed.push_back(project(list->candidate(i).text().toString()));
    const auto displayedPreedit = context.inputPanel().preedit().toString();

    converted = false;
    assertTrue(!ui.handleButtonPress(120, 20), "switching back rejects the old filtered frame");
    ui.update(fcitx::UserInterfaceComponent::InputPanel, &context);
    paint(ui);
    assertTrue(list->size() == rawPageSize && capture(ui) == rawPixels,
               "switching back restores original text and width without a stale projection cache");
    converted = true;
    ui.update(fcitx::UserInterfaceComponent::InputPanel, &context);
    paint(ui);
    assertTrue(ui.handleButtonPress(120, 20) &&
                   context.commits == std::vector<std::string>{"软件0"} &&
                   controller.page().rawInput == "ma",
               "widened hit target selects the original candidate and exact consumed input");

    // The filtered frame must match a frame whose input already contains the
    // expected display strings, proving that the production renderer consumes
    // the projection rather than only measuring it.
    converted = false;
    auto expectedList = std::make_unique<fcitx::CommonCandidateList>();
    for (std::size_t i = 0; i < displayed.size(); ++i)
        expectedList->append(std::make_unique<TestCandidateWord>(
            displayed[i], static_cast<int>(i), nullptr));
    expectedList->setGlobalCursorIndex(0);
    context.inputPanel().setCandidateList(std::move(expectedList));
    context.inputPanel().setPreedit(fcitx::Text(displayedPreedit));
    ui.update(fcitx::UserInterfaceComponent::InputPanel, &context);
    paint(ui);
    assertTrue(projectedPixels != rawPixels && capture(ui) == projectedPixels,
               "production drawing displays the exact output-filter projection");
    std::cout << "[PASS] output projection, width, live switch and original selection\n";
}

void testWorkarea(fcitx::Instance &instance) {
    TestInputContext context(instance.inputContextManager());
    context.focusIn();
    context.setCursorRect(fcitx::Rect(10, 20, 1, 20));
    modernime::ui::ModernIMEUserInterface ui(&instance);
    PrefixProvider provider;
    provider.texts.clear();
    for (int i = 0; i < 18; ++i) provider.texts.push_back("你好" + std::to_string(i));
    modernime::fcitx5::FcitxEngineHost host(context);
    modernime::fcitx5::ModernIMEController controller(host, &provider);
    host.setController(controller);
    const auto type = [&] {
        for (const char c : std::string("nihaoma"))
            controller.handle({modernime::fcitx5::KeyKind::Character, c});
        ui.update(fcitx::UserInterfaceComponent::InputPanel, &context);
        paint(ui);
    };
    type();
    auto *area = candidateArea();
    auto *window = GTK_WINDOW(gtk_widget_get_toplevel(area));
    auto *display = gtk_widget_get_display(area);
    GdkRectangle workarea{};
    gdk_monitor_get_workarea(gdk_display_get_monitor_at_point(display, 10, 40), &workarea);
    int width = 0, height = 0;
    gtk_widget_get_size_request(area, &width, &height);
    std::cout << "WORKAREA " << workarea.width << " REQUEST " << width << '\n';
    assertTrue(width <= workarea.width, "candidate window fits monitor workarea");

    const auto candidates = [&] {
        return std::dynamic_pointer_cast<modernime::fcitx5::FcitxCandidateList>(
            context.inputPanel().candidateList());
    };
    auto list = candidates();
    assertTrue(list != nullptr, "uses real controller candidate list");
    std::size_t next = 0;
    for (const auto &page : list->pageBoundaries()) {
        assertTrue(page.begin == next && page.end > page.begin,
                   "narrow pages cover candidates in order without gaps");
        next = page.end;
    }
    assertTrue(next == provider.texts.size() && list->totalPages() > 1,
               "all candidates remain reachable through pagination");

    // A cursor-only event must move the visible panel without another input key.
    context.setCursorRect(fcitx::Rect(workarea.x + workarea.width - 2,
                                      workarea.y + workarea.height - 20, 1, 20));
    gdk_display_sync(display);
    drainGtkEvents();
    int x = 0, y = 0;
    gtk_window_get_size(window, &width, &height);
    gtk_window_get_position(window, &x, &y);
    std::cout << "WINDOW " << width << ' ' << height << " POSITION " << x << ' ' << y << '\n';
    assertTrue(x >= workarea.x && x + width <= workarea.x + workarea.width &&
                   y >= workarea.y && y + height <= workarea.y + workarea.height && y > 40,
               "cursor-only movement follows caret and stays in workarea");

    while (candidates()->hasNext()) {
        controller.handle({modernime::fcitx5::KeyKind::NextPage});
        ui.update(fcitx::UserInterfaceComponent::InputPanel, &context);
    }
    paint(ui);
    const auto mouseText = candidates()->candidate(0).text().toString();
    click();
    assertTrue(context.commits == std::vector<std::string>{mouseText} &&
                   controller.page().rawInput == "ma",
               "last narrow page mouse selection commits exact identity and consumption");
    controller.handle({modernime::fcitx5::KeyKind::Escape});
    type();
    controller.handle({modernime::fcitx5::KeyKind::NextPage});
    ui.update(fcitx::UserInterfaceComponent::InputPanel, &context);
    const auto keyboardText = candidates()->candidate(0).text().toString();
    controller.handle({modernime::fcitx5::KeyKind::Digit, 0, '1'});
    assertTrue(context.commits == std::vector<std::string>{mouseText, keyboardText} &&
                   controller.page().rawInput == "ma",
               "narrow page number selection matches displayed identity and consumption");
    std::cout << "[PASS] workarea pagination, cursor movement and selection\n";
}

void testScaledCursor(fcitx::Instance &instance) {
    TestInputContext context(instance.inputContextManager());
    context.focusIn();
    modernime::ui::ModernIMEUserInterface ui(&instance);
    auto *area = candidateArea();
    const int scale = gtk_widget_get_scale_factor(area);
    PrefixProvider provider;
    modernime::fcitx5::FcitxEngineHost host(context);
    modernime::fcitx5::ModernIMEController controller(host, &provider);
    host.setController(controller);
    // X11 frontends send root coordinates in physical pixels, with scale=1
    // in the Fcitx context even when the application is displayed at 200%.
    context.setCursorRect(fcitx::Rect(80 * scale, 80 * scale,
                                      81 * scale, 100 * scale));
    for (const char c : std::string("nihaoma"))
        controller.handle({modernime::fcitx5::KeyKind::Character, c});
    ui.update(fcitx::UserInterfaceComponent::InputPanel, &context);
    paint(ui);
    gdk_display_sync(gtk_widget_get_display(area));
    drainGtkEvents();
    int x = 0, y = 0;
    gtk_window_get_position(GTK_WINDOW(gtk_widget_get_toplevel(area)), &x, &y);
    std::cout << "SCALE " << scale << " PHYSICAL_CURSOR " << 80 * scale << ' '
              << 100 * scale << " LOGICAL_WINDOW " << x << ' ' << y << '\n';
    assertTrue(x == 80 && y == 100,
               "physical caret maps once to GTK logical coordinates");
#ifdef GDK_WINDOWING_X11
    const int nextScale = scale == 1 ? 2 : 1;
    gdk_window_freeze_updates(gtk_widget_get_window(area));
    gdk_x11_display_set_window_scale(gtk_widget_get_display(area), nextScale);
    assertTrue(gtk_widget_get_scale_factor(area) == nextScale,
               "the actual GTK display changes scale while composing");
    assertTrue(!ui.handleButtonPress(30, 20) && context.commits.empty(),
               "scale change revokes old drawing selection before repaint");
    gdk_window_thaw_updates(gtk_widget_get_window(area));
    paint(ui);
    gdk_display_sync(gtk_widget_get_display(area));
    drainGtkEvents();
    gtk_window_get_position(GTK_WINDOW(gtk_widget_get_toplevel(area)), &x, &y);
    std::cout << "RESCALED " << nextScale << " LOGICAL_WINDOW " << x << ' ' << y << '\n';
    assertTrue(x == 80 * scale / nextScale && y == 100 * scale / nextScale,
               "live scale change follows the same physical caret without a new key");
#endif
    click();
    assertTrue(context.commits == std::vector<std::string>{"你好"} &&
                   controller.page().rawInput == "ma",
               "scaled mouse selection retains candidate identity and consumption");
#ifdef GDK_WINDOWING_X11
    gdk_x11_display_set_window_scale(gtk_widget_get_display(area), scale);
#endif
}

void testSelection(fcitx::Instance &instance, std::string_view scenario) {
    int selected = -1;
    auto context = std::make_unique<TestInputContext>(instance.inputContextManager());
    context->focusIn();
    modernime::ui::ModernIMEUserInterface ui(&instance);
    auto list = words(&selected);
    auto *mutableList = list.get();
    context->inputPanel().setCandidateList(std::move(list));
    ui.update(fcitx::UserInterfaceComponent::InputPanel, context.get());
    paint(ui);

    if (scenario == "replacement" || scenario == "unpainted") {
        context->inputPanel().setCandidateList(words(&selected));
        if (scenario == "unpainted") {
            // Block only GTK draw handlers, leaving production update and hit-testing intact.
            GList *windows = gtk_window_list_toplevels();
            for (auto *it = windows; it; it = it->next) {
                auto *child = gtk_bin_get_child(GTK_BIN(it->data));
                if (child && GTK_IS_DRAWING_AREA(child)) {
                    g_signal_handlers_block_matched(child, G_SIGNAL_MATCH_ID,
                        g_signal_lookup("draw", GTK_TYPE_WIDGET), 0, nullptr, nullptr, nullptr);
                }
            }
            g_list_free(windows);
            ui.update(fcitx::UserInterfaceComponent::InputPanel, context.get());
        }
    } else if (scenario == "queued") {
        struct QueuedClick {
            GtkWidget *area;
            bool queued = false;
            bool painted = false;
        } queued{candidateArea()};
        auto *clock = gtk_widget_get_frame_clock(queued.area);
        assertTrue(clock != nullptr, "real GTK frame clock available");
        const auto before = g_signal_connect(clock, "before-paint",
            G_CALLBACK(+[](GdkFrameClock *, gpointer data) {
                auto &state = *static_cast<QueuedClick *>(data);
                if (!state.queued) {
                    auto *event = buttonEvent(state.area);
                    gdk_event_put(event);
                    gdk_event_free(event);
                    state.queued = true;
                }
            }), &queued);
        const auto after = g_signal_connect(clock, "after-paint",
            G_CALLBACK(+[](GdkFrameClock *, gpointer data) {
                static_cast<QueuedClick *>(data)->painted = true;
            }), &queued);
        auto replacement = words(&selected);
        const_cast<TestCandidateWord &>(static_cast<const TestCandidateWord &>(
            replacement->candidate(0))).changeText("新页候选");
        context->inputPanel().setCandidateList(std::move(replacement));
        ui.update(fcitx::UserInterfaceComponent::InputPanel, context.get());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!queued.painted && std::chrono::steady_clock::now() < deadline) {
            if (!g_main_context_iteration(nullptr, FALSE)) g_usleep(1000);
        }
        drainGtkEvents();
        g_signal_handler_disconnect(clock, before);
        g_signal_handler_disconnect(clock, after);
        assertTrue(queued.queued && queued.painted, "old click crosses real GTK before-paint/draw/after-paint");
        assertTrue(context->commits.empty() && selected == -1,
                   "queued old click must not select newly painted page");
        click();
        assertTrue(context->commits == std::vector<std::string>{"新页候选"},
                   "new click after event drain selects new page exactly once");
        std::cout << "[PASS] selection " << scenario << '\n';
        return;
    } else if (scenario == "page") {
        mutableList->setPage(1);
    } else if (scenario == "focus") {
        context->focusOut();
        context->focusIn();
    } else if (scenario == "destroy") {
        context.reset();
    } else if (scenario == "reentrant") {
        auto &word = const_cast<TestCandidateWord &>(
            static_cast<const TestCandidateWord &>(mutableList->candidate(0)));
        word.afterSelect = [&] { context.reset(); };
        click();
        assertTrue(!context && selected == 0, "selection can destroy its owner safely");
        click();
        assertTrue(selected == 0, "destroyed owner cannot select again");
        std::cout << "[PASS] selection " << scenario << '\n';
        return;
    } else if (scenario == "suspend") {
        ui.suspend();
        click();
        assertTrue(selected == -1, "suspended UI rejects click");
        ui.resume();
    } else if (scenario == "hidden") {
        context->inputPanel().reset();
        ui.update(fcitx::UserInterfaceComponent::InputPanel, context.get());
        context->inputPanel().setCandidateList(words(&selected));
    } else if (scenario == "mutated-text") {
        const_cast<TestCandidateWord &>(static_cast<const TestCandidateWord &>(mutableList->candidate(0))).changeText("未绘制");
    } else if (scenario == "partial" || scenario == "same-text") {
        PrefixProvider provider;
        modernime::fcitx5::FcitxEngineHost host(*context);
        modernime::fcitx5::ModernIMEController controller(host, &provider);
        host.setController(controller);
        for (const char c : std::string("nihaoma")) {
            controller.handle({modernime::fcitx5::KeyKind::Character, c});
        }
        ui.update(fcitx::UserInterfaceComponent::InputPanel, context.get());
        paint(ui);
        if (scenario == "same-text") {
            provider.consumed = 2;
            controller.handle({modernime::fcitx5::KeyKind::Character, 'a'});
            click();
            assertTrue(context->commits.empty(), "same text with new consumption cannot use old geometry");
            assertTrue(controller.page().rawInput == "nihaomaa", "stale click preserves full input");
            ui.update(fcitx::UserInterfaceComponent::InputPanel, context.get());
            paint(ui);
        }
        click();
        assertTrue(context->commits == std::vector<std::string>{"你好"}, "real controller commits exact displayed text once");
        assertTrue(controller.page().rawInput == (scenario == "partial" ? "ma" : "haomaa"),
                   "mouse selection preserves exact remaining input");
        std::cout << "[PASS] selection " << scenario << '\n';
        return;
    } else {
        assertTrue(false, "unknown selection scenario");
    }
    click();
    assertTrue(selected == -1, "stale selection rejected: " + std::string(scenario));
    if (context) {
        assertTrue(context->commits.empty(), "stale page never commits text");
        ui.update(fcitx::UserInterfaceComponent::InputPanel, context.get());
        paint(ui);
        click();
        assertTrue(selected == (scenario == "page" ? 5 : 0), "freshly drawn page selects intended object");
        assertTrue(context->commits == std::vector<std::string>{
            scenario == "page" ? "新词" : scenario == "mutated-text" ? "未绘制" : "旧词"},
            "freshly drawn page commits exact text once");
    }
    std::cout << "[PASS] selection " << scenario << '\n';
}

} // namespace

int main(int argc, char *argv[]) {
    // Initialize GTK; if display is unavailable, exit with skip code 77
    if (!gtk_init_check(&argc, &argv)) {
        std::cout << "[SKIP] modernime_ui_addon_tests: GTK display unavailable (no DISPLAY or WAYLAND_DISPLAY)\n";
        return 77;
    }

    char templatePath[] = "/tmp/modernime-ui-addon-test-XXXXXX";
    char *tempDir = mkdtemp(templatePath);
    assertTrue(tempDir != nullptr, "Failed to create temporary directory");
    const std::filesystem::path testDir = tempDir;

    setenv("XDG_CONFIG_HOME", testDir.c_str(), 1);
    setenv("XDG_DATA_HOME", testDir.c_str(), 1);

    const auto configDir = testDir / "modernime";
    std::filesystem::create_directories(configDir);
    const auto settingsFile = configDir / "settings.conf";

    // 1. Initial settings with candidate.font_size = 14
    modernime::core::ModernIMESettings initialSettings = modernime::core::defaultSettings();
    initialSettings.candidateFontSize = 14;
    initialSettings.candidatePageSize = 5;
    const bool workareaTest = argc == 2 && std::string_view(argv[1]) == "--workarea";
    if (workareaTest) {
        initialSettings.candidateFontSize = 28;
        initialSettings.candidatePageSize = 9;
    }
    assertTrue(modernime::core::SettingsStore::save(settingsFile, initialSettings),
               "Failed to write initial settings");

    char programName[] = "modernime_ui_addon_tests";
    char disableArg[] = "--disable=all";
    char *fcitxArgv[] = {programName, disableArg, nullptr};
    optind = 0;
    fcitx::Instance instance(2, fcitxArgv);
    instance.initialize();

    if (argc == 2 && std::string_view(argv[1]) == "--scale") {
        testScaledCursor(instance);
        std::filesystem::remove_all(testDir);
        return 0;
    }
    if (workareaTest) {
        testWorkarea(instance);
        std::filesystem::remove_all(testDir);
        return 0;
    }

    if (argc == 3 && std::string_view(argv[1]) == "--selection-case") {
        testSelection(instance, argv[2]);
        std::filesystem::remove_all(testDir);
        return 0;
    }

    testOutputProjection(instance);
    testScaledCursor(instance);

    {
    TestInputContext context(instance.inputContextManager());
    context.focusIn();
    int selectedIndex = -1;

    auto candidateList = std::make_unique<fcitx::CommonCandidateList>();
    candidateList->append(std::make_unique<TestCandidateWord>("你好", 0, &selectedIndex));
    candidateList->append(std::make_unique<TestCandidateWord>("拟好", 1, &selectedIndex));
    candidateList->append(std::make_unique<TestCandidateWord>("泥蒿", 2, &selectedIndex));
    candidateList->append(std::make_unique<TestCandidateWord>("拟稿", 3, &selectedIndex));
    candidateList->append(std::make_unique<TestCandidateWord>("呢好", 4, &selectedIndex));
    context.inputPanel().setCandidateList(std::move(candidateList));

    // Construct production UI addon
    modernime::ui::ModernIMEUserInterface uiAddon(&instance);

    // Initial update to measure and create layout
    uiAddon.update(fcitx::UserInterfaceComponent::InputPanel, &context);

    paint(uiAddon);

    // 2. Perform click on first candidate area
    // In horizontal layout at font size 14, (x=30, y=14) hits candidate 0
    selectedIndex = -1;
    bool handled = uiAddon.handleButtonPress(30.0, 14.0);
    assertTrue(handled, "handleButtonPress should return true");
    assertTrue(selectedIndex == 0, "Candidate 0 should be selected with initial layout");

    // 3. Update configuration on disk: change font size to 28
    // This alters font metrics and requires layout invalidation
    modernime::core::ModernIMESettings newSettings = initialSettings;
    newSettings.candidateFontSize = 28;
    // Sleep to exceed the 1500ms throttling window in refreshSettingsIfNeeded
    g_usleep(1600000);
    assertTrue(modernime::core::SettingsStore::save(settingsFile, newSettings),
               "Failed to update settings file on disk");

    // 4. Test layout invalidation:
    // Before calling update(), call handleButtonPress().
    // handleButtonPress() refreshes settings, detects stamp change, and updates metrics.
    // The previous layout has old layoutGeneration, so hitTestCandidate() MUST reject it!
    selectedIndex = -1;
    uiAddon.handleButtonPress(30.0, 14.0);
    assertTrue(selectedIndex == -1,
               "Click with stale layout generation must be rejected when metrics changed");

    // 5. Now update UI: this remeasures layout with new metrics (font size 28)
    uiAddon.update(fcitx::UserInterfaceComponent::InputPanel, &context);

    paint(uiAddon);

    // 6. Click on candidate with new layout:
    // With font size 28, candidate 0 and candidate 1 are wider.
    // Click candidate 0
    selectedIndex = -1;
    uiAddon.handleButtonPress(30.0, 20.0);
    assertTrue(selectedIndex == 0,
               "Candidate 0 should be selected in updated layout");

    // Verify candidate semantics and text
    auto activeList = context.inputPanel().candidateList();
    assertTrue(activeList != nullptr && activeList->size() == 5,
               "Candidate list preserved after layout refresh");
    assertTrue(activeList->candidate(selectedIndex).text().toString() == "你好",
               "Selected candidate text semantics preserved");

    // Change only the new appearance settings: same size, fresh geometry.
    newSettings.candidateFontFamily = "DejaVu Sans";
    newSettings.candidateTheme = modernime::core::CandidateTheme::Dark;
    newSettings.candidateSpacing = modernime::core::CandidateSpacing::Compact;
    g_usleep(1600000);
    assertTrue(modernime::core::SettingsStore::save(settingsFile, newSettings), "save new appearance");
    selectedIndex = -1;
    uiAddon.handleButtonPress(30.0, 20.0);
    assertTrue(selectedIndex == -1, "font/theme/spacing invalidate the previous painted frame");
    uiAddon.update(fcitx::UserInterfaceComponent::InputPanel, &context);
    paint(uiAddon);
    auto *appearanceSurface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1000, 120);
    auto *appearanceContext = cairo_create(appearanceSurface);
    uiAddon.draw(appearanceContext);
    cairo_surface_flush(appearanceSurface);
    const auto *pixels = cairo_image_surface_get_data(appearanceSurface);
    const auto *row = reinterpret_cast<const std::uint32_t *>(pixels + 5 * cairo_image_surface_get_stride(appearanceSurface));
    assertTrue((row[200] >> 24) == 255 && ((row[200] >> 16) & 255) > 20 &&
                   ((row[200] >> 16) & 255) < 64, "production candidate panel actually renders the dark theme");
    if (const auto *capture = std::getenv("MODERNIME_TEST_APPEARANCE_CAPTURE"))
        assertTrue(cairo_surface_write_to_png(appearanceSurface, capture) == CAIRO_STATUS_SUCCESS, "capture production appearance");
    cairo_destroy(appearanceContext);
    cairo_surface_destroy(appearanceSurface);
    drainGtkEvents();
    uiAddon.handleButtonPress(30.0, 20.0);
    assertTrue(selectedIndex == 0 && context.commits.back() == "你好", "new appearance preserves the displayed selection");

    // A material-only change must invalidate the old painted frame too.
    newSettings.candidateStyle = modernime::core::CandidateStyle::Textured;
    g_usleep(1600000);
    assertTrue(modernime::core::SettingsStore::save(settingsFile, newSettings), "save candidate material only");
    selectedIndex = -1;
    uiAddon.handleButtonPress(30.0, 20.0);
    assertTrue(selectedIndex == -1, "candidate material reload invalidates old painted selection");
    uiAddon.update(fcitx::UserInterfaceComponent::InputPanel, &context);
    paint(uiAddon);
    uiAddon.handleButtonPress(30.0, 20.0);
    assertTrue(selectedIndex == 0 && context.commits.back() == "你好",
               "textured candidate keeps the same displayed selection after repaint");

    context.focusOut();
    }
    for (const auto *scenario : {"replacement", "page", "unpainted", "focus", "destroy",
                                  "hidden", "mutated-text", "partial", "same-text", "reentrant", "suspend", "queued"}) {
        testSelection(instance, scenario);
    }
    initialSettings.candidateFontSize = 28;
    initialSettings.candidatePageSize = 9;
    assertTrue(modernime::core::SettingsStore::save(settingsFile, initialSettings),
               "Failed to write workarea test settings");
    testWorkarea(instance);

    // Cleanup
    std::filesystem::remove_all(testDir);

    std::cout << "[PASS] modernime_ui_addon_tests: config refresh, layout invalidation, and candidate selection verified\n";
    return 0;
}
