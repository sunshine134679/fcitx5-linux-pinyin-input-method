#pragma once

#include <gtk/gtk.h>

namespace modernime::settings {

// Pack a disclosure title and its content as siblings, so GtkExpander does not
// hide the content before the shared closing transition can finish.
GtkWidget *createSettingsDisclosure(GtkWidget *controller, GtkWidget *content);

// controller is a GtkToggleButton or a GtkExpander used as a title. Content
// remains in the revealer; an optional existing arrow turns with the disclosure.
void bindSettingsDisclosure(GtkWidget *controller, GtkWidget *revealer,
                            GtkWidget *arrow = nullptr,
                            bool halfTurnArrow = false);
void setSettingsDisclosureExpanded(GtkWidget *controller, bool expanded,
                                   bool animate = true);
void setSettingsRevealed(GtkWidget *revealer, bool expanded,
                         bool animate = true);

} // namespace modernime::settings
