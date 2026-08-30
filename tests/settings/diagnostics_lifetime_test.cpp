#include "modernime/settings/detail/diagnostics_lifetime.h"

#include <cassert>
#include <memory>

namespace {

struct FakePage final {
    int refreshes = 0;
};

} // namespace

int main() {
    using modernime::settings::detail::DiagnosticsLifetime;

    FakePage page;
    auto pageState = std::make_shared<DiagnosticsLifetime<FakePage>>(&page);
    auto delayedCallbackState = pageState;

    assert(pageState->withOwner(
        [](FakePage &owner) { ++owner.refreshes; }));
    assert(page.refreshes == 1);

    pageState->deactivate();
    pageState.reset();

    assert(!delayedCallbackState->withOwner(
        [](FakePage &owner) { ++owner.refreshes; }));
    assert(page.refreshes == 1);
}
