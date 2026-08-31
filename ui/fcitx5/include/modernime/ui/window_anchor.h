#pragma once

namespace modernime::ui {

struct WindowAnchor final {
    int x = 0;
    int y = 0;
    bool valid = false;

    // 记录新位置；仅在坐标实际变化时返回 true，调用方据此刻意移动窗口，
    // 避免每次更新都触发一次合成器重排。
    bool capture(int nextX, int nextY) {
        if (valid && x == nextX && y == nextY) {
            return false;
        }
        x = nextX;
        y = nextY;
        valid = true;
        return true;
    }

    void reset() {
        x = 0;
        y = 0;
        valid = false;
    }
};

} // namespace modernime::ui