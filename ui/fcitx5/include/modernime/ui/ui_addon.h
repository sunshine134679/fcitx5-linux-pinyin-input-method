#pragma once

#include <fcitx/userinterface.h>

#include <cairo/cairo.h>

#include <memory>

namespace modernime::ui {

class ModernIMEUserInterface final : public fcitx::UserInterface {
public:
    ModernIMEUserInterface();
    ~ModernIMEUserInterface() override;

    void update(fcitx::UserInterfaceComponent component,
                fcitx::InputContext *inputContext) override;
    bool available() override;
    void suspend() override;
    void resume() override;
    void draw(cairo_t *context);

private:
    struct Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::ui
