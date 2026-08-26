#include "modernime/fcitx5/fcitx_engine.h"

#include <fcitx/addonfactory.h>

namespace {

class ModernIMEAddonFactory final : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new modernime::fcitx5::ModernIMEInputMethod(manager);
    }
};

} // namespace

FCITX_ADDON_FACTORY(ModernIMEAddonFactory)
