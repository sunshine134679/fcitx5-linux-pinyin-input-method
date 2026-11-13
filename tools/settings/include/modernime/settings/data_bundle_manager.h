#pragma once
#include <gtk/gtk.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
namespace modernime::settings {
class DataBundleManager final {
public:
    DataBundleManager(std::function<bool()> canRestore, std::function<void()> restored);
    ~DataBundleManager();
    GtkWidget *widget() const;
    bool focusTarget(std::string_view target);
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
