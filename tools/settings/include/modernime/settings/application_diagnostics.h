#pragma once

#include <gtk/gtk.h>

#include <memory>
#include <string_view>

namespace modernime::settings {

// Read-only facts from the active input method; no application settings are changed.
class ApplicationDiagnostics final {
public:
    ApplicationDiagnostics();
    ~ApplicationDiagnostics();
    ApplicationDiagnostics(const ApplicationDiagnostics &) = delete;
    ApplicationDiagnostics &operator=(const ApplicationDiagnostics &) = delete;

    GtkWidget *widget() const;
    void refresh();
    bool focusTarget(std::string_view target);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace modernime::settings
