#pragma once

namespace modernime::ui {

struct WindowAnchor final {
    int x = 0;
    int y = 0;
    bool latched = false;

    bool capture(int nextX, int nextY) {
        if (latched) {
            return false;
        }
        x = nextX;
        y = nextY;
        latched = true;
        return true;
    }

    void reset() {
        x = 0;
        y = 0;
        latched = false;
    }
};

} // namespace modernime::ui
