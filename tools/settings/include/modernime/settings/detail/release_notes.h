#pragma once

#include <string>
#include <string_view>

namespace modernime::settings::detail {

// Release notes are displayed by a selectable native GtkLabel. Source HTML is
// always escaped; only Markdown formatting and web links become label markup.
std::string releaseNotesMarkup(std::string_view markdown,
                               std::string_view version);

} // namespace modernime::settings::detail
