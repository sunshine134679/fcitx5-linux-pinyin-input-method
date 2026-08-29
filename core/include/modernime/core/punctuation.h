#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace modernime::core {

// Paired quotes are stateful (open/close side depends on how many were
// committed); the controller tracks that state and uses these constants.
inline constexpr std::string_view kLeftDoubleQuote = "“";
inline constexpr std::string_view kRightDoubleQuote = "”";
inline constexpr std::string_view kLeftSingleQuote = "‘";
inline constexpr std::string_view kRightSingleQuote = "’";

// Fixed full-width replacement for an ASCII punctuation character in Chinese
// mode; nullopt when the character has no conversion and should stay
// half-width (for example '@' or '/').
std::optional<std::string> fullWidthPunctuation(char ascii);

} // namespace modernime::core
