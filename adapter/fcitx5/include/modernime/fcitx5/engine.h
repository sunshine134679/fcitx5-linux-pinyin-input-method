#pragma once

#include "modernime/core/candidate_model.h"
#include "modernime/core/candidate_provider.h"
#include "modernime/core/input_state.h"

#include <string_view>

namespace modernime::fcitx5 {

enum class KeyKind {
    Character,
    Backspace,
    Escape,
    Enter,
    Space,
    Digit,
    Toggle,
    PreviousCandidate,
    NextCandidate,
    PreviousPage,
    NextPage,
};

struct KeyEvent final {
    KeyKind kind = KeyKind::Character;
    char character = 0;
    char digit = 0;
};

class EngineHost {
public:
    virtual ~EngineHost() = default;

    virtual void publishPage(const core::CandidatePage &page) = 0;
    virtual void commit(std::string_view text) = 0;
};

class ModernIMEController final {
public:
    explicit ModernIMEController(EngineHost &host,
                                 core::CandidateProvider *provider = nullptr);

    bool handle(const KeyEvent &event);
    bool select(std::size_t index);
    void reset();
    void setActive(bool active);

    bool active() const { return active_; }
    const core::CandidatePage &page() const { return page_; }

private:
    void refreshPage();
    bool commitCurrent();
    bool moveCursor(std::ptrdiff_t delta);

    EngineHost &host_;
    core::CandidateProvider *provider_ = nullptr;
    core::InputState input_;
    core::CandidatePage page_;
    bool active_ = true;
};

} // namespace modernime::fcitx5
