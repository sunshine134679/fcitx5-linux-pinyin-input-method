#include "modernime/core/input_compatibility.h"
#include <algorithm>

namespace modernime::core {
void InputCompatibilityTracker::observe(InputCompatibilityFacts facts) {
    if (facts.application.empty() || facts.application.size() > 256) return;
    if (facts.frontend.size() > 64 || facts.display.size() > 256) return;
    if (facts.sensitive) facts.surroundingText = false;
    std::lock_guard lock(mutex_);
    latest_ = facts.application;
    if (!reports_.contains(latest_) && reports_.size() == 32) {
        const auto oldest = std::min_element(reports_.begin(), reports_.end(), [](const auto &a, const auto &b) {
            return a.second.facts.observedAtMs < b.second.facts.observedAtMs;
        });
        reports_.erase(oldest);
    }
    auto &report = reports_[latest_];
    report.observed = true;
    report.facts = std::move(facts);
    report.transport = report.facts.display.starts_with("x11:") ? "x11" :
        report.facts.display.starts_with("wayland:") ? "wayland" : "unknown";
    report.suggestions.clear();
    if (report.facts.sensitive) report.suggestions.push_back("敏感输入：学习、周边文本与剪贴板采集已受隐私边界限制");
    if (!report.facts.modernimeSelected) report.suggestions.push_back("在目标应用切换到 ModernIME 后重新试输入");
    if (!report.facts.focused) report.suggestions.push_back("目标应用目前没有输入焦点，请聚焦输入框后重新检测");
    if (!report.facts.surroundingText && !report.facts.sensitive)
        report.suggestions.push_back("应用未提供周边文本能力；成对标点、撤销重选与宿主语境功能可能受限");
    if (!report.facts.cursorRectangle) report.suggestions.push_back("尚未观察到有效光标矩形；请在目标输入框输入后检查候选位置");
    if (report.transport == "unknown") report.suggestions.push_back("显示协议未能由目标输入上下文确定，不根据全局环境变量猜测");
}
void InputCompatibilityTracker::recordKey(const std::string &application, bool handled,
                                          std::uint64_t dispatchUs) {
    std::lock_guard lock(mutex_);
    const auto it = reports_.find(application);
    if (it == reports_.end() || it->second.facts.sensitive) return;
    auto &report = it->second;
    if (handled) ++report.handledKeys;
    report.maximumDispatchUs = std::max(report.maximumDispatchUs, dispatchUs);
}
InputCompatibilityReport InputCompatibilityTracker::report(const std::string &application) const {
    std::lock_guard lock(mutex_);
    const auto it = reports_.find(application.empty() ? latest_ : application);
    if (it != reports_.end()) return it->second;
    InputCompatibilityReport result;
    result.facts.application = application;
    result.suggestions.push_back("未观察到此应用的 Fcitx 输入上下文，请在目标应用实际试输入");
    return result;
}
std::vector<std::string> InputCompatibilityTracker::applications() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> result;
    for (const auto &[application, report] : reports_) { (void)report; result.push_back(application); }
    return result;
}
} // namespace modernime::core
