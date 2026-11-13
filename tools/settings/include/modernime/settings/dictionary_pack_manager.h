#pragma once

#include <gtk/gtk.h>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace modernime::settings {

class DictionaryPackManager final {
public:
    explicit DictionaryPackManager(std::function<void(std::string)> notify);
    ~DictionaryPackManager();
    GtkWidget *widget() const;
    void refresh();
    bool focusTarget(std::string_view target);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::settings
