#include "modernime/core/punctuation.h"

namespace modernime::core {

std::optional<std::string> fullWidthPunctuation(char ascii) {
    switch (ascii) {
    case ',':
        return std::string("，");
    case '.':
        return std::string("。");
    case '?':
        return std::string("？");
    case '!':
        return std::string("！");
    case ':':
        return std::string("：");
    case ';':
        return std::string("；");
    case '(':
        return std::string("（");
    case ')':
        return std::string("）");
    case '~':
        return std::string("～");
    case '/':
    case '\\':
        return std::string("、");
    case '<':
        return std::string("《");
    case '>':
        return std::string("》");
    case '[':
        return std::string("【");
    case ']':
        return std::string("】");
    case '^':
        return std::string("……");
    case '_':
        return std::string("——");
    case '$':
        return std::string("￥");
    default:
        return std::nullopt;
    }
}

} // namespace modernime::core
