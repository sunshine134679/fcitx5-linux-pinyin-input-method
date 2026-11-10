#include "modernime/fcitx5/engine.h"
#include <cstdlib>
#include <iostream>
using namespace modernime;
namespace {
void expect(bool value, const char *message) { if (!value) { std::cerr << message << '\n'; std::exit(1); } }
struct Host final : fcitx5::EngineHost {
    std::string committed;
    void publishPage(const core::CandidatePage &) override {}
    void commit(std::string_view text) override { committed += text; }
};
core::ClipboardSnapshot snapshot(std::vector<std::string> history, std::vector<std::string> favorites) {
    core::ClipboardSnapshot result;
    result.entries = std::make_shared<const std::vector<std::string>>(std::move(history));
    result.favorites = std::make_shared<const std::vector<std::string>>(std::move(favorites));
    result.availability = result.favoritesAvailability = core::ClipboardAvailability::Ready;
    return result;
}
}
int main() {
    using fcitx5::KeyKind;
    Host host; fcitx5::ModernIMEController controller(host);
    const auto initial = snapshot({"Alpha42", "中文地址", "Beta"}, {"常用回复", "Alpha42"});
    controller.setClipboardSnapshot(initial);
    expect(controller.handle({KeyKind::OpenClipboard}) && controller.page().items.size() == 4 &&
           controller.page().items[0].text == "常用回复" && controller.page().items[1].annotation == "收藏", "favorites merge first without duplicates");
    auto old = controller.page();
    core::CandidateSelectionRequest stale{old.sessionId, old.displayGeneration, 0, core::candidateIdentity(old.items[0], old)};
    expect(controller.handle({KeyKind::StartClipboardSearch}), "start clipboard search");
    for (char c : std::string("ALPHA4")) controller.handle({KeyKind::Character,c});
    expect(controller.page().items.size() == 1 && controller.page().items[0].text == "Alpha42" &&
           !controller.select(stale), "case-insensitive query preserves digit and invalidates old selection");
    expect(controller.setClipboardQuery("中文") && controller.page().items[0].text == "中文地址", "Chinese query API");
    controller.handle({KeyKind::Enter});
    expect(host.committed == "中文地址" && !controller.clipboardMode(), "filtered candidate commits full text");
    controller.setClipboardSnapshot(initial); controller.handle({KeyKind::OpenClipboard});
    std::string requested; bool favorite = true; int calls = 0;
    controller.setClipboardFavoriteHandler([&](std::string_view text, bool add) -> std::uint64_t { requested = text; favorite = add; ++calls; return 7; });
    expect(controller.favoriteCurrentClipboard() && requested == "常用回复" && !favorite && controller.clipboardFavoritePending(), "favorite mutation uses full current text and receives operation identity");
    expect(!controller.favoriteCurrentClipboard() && calls == 1, "pending persistence does not queue duplicate toggles");
    auto removed = snapshot({"Alpha42", "中文地址", "Beta"}, {"Alpha42"}); removed.lastFavoriteOperationId = 7;
    controller.setClipboardSnapshot(removed);
    expect(!controller.clipboardFavoritePending() && controller.page().items[0].text == "Alpha42", "completed snapshot confirms favorite change");
    expect(controller.setClipboardQuery("", true) && controller.page().items.size() == 1, "favorites-only query");
    auto cleared = snapshot({}, {"Alpha42"}); cleared.lastFavoriteOperationId = 7;
    controller.setClipboardSnapshot(cleared);
    expect(controller.page().items.size() == 1 && controller.page().items[0].text == "Alpha42", "history clear retains favorites");
    controller.setClipboardQuery("no match"); controller.handle({KeyKind::Enter});
    expect(host.committed == "中文地址" && controller.clipboardSearching(), "search hint never commits");
    controller.handle({KeyKind::Escape});
    expect(controller.clipboardMode() && !controller.clipboardSearching() && controller.page().preedit.empty(), "first escape exits search");
    controller.handle({KeyKind::Escape}); expect(!controller.clipboardMode(), "second escape closes history");
    controller.handle({KeyKind::OpenClipboard}); controller.setClipboardQuery("Alpha"); controller.setSensitive(true);
    expect(!controller.clipboardMode() && !controller.setClipboardQuery("Alpha") && !controller.favoriteCurrentClipboard(), "sensitive transition drops query and rejects new search or favorite request");
    std::cout << "clipboard search controller test passed\n";
}
