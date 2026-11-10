#include "modernime/core/calculator.h"

#include <cmath>
#include <cstddef>
#include <iomanip>
#include <locale>
#include <sstream>
#include <utility>

namespace modernime::core {
namespace {

constexpr std::size_t kMaxExpressionBytes = 512;
constexpr unsigned kMaxParenthesisDepth = 32;

bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

// Convert the already rounded classic-locale spelling to ordinary decimal.
std::string formatResult(long double value) {
    if (value == 0) {
        return "0";
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(15) << value;
    auto text = output.str();
    const auto exponentAt = text.find('e');
    if (exponentAt == std::string::npos) {
        return text;
    }

    const int exponent = std::stoi(text.substr(exponentAt + 1));
    const bool negative = text.front() == '-';
    auto digits = text.substr(negative ? 1 : 0,
                              exponentAt - (negative ? 1 : 0));
    const auto dotAt = digits.find('.');
    int decimalAt = static_cast<int>(dotAt == std::string::npos
                                         ? digits.size()
                                         : dotAt);
    if (dotAt != std::string::npos) {
        digits.erase(dotAt, 1);
    }
    decimalAt += exponent;
    if (decimalAt <= 0) {
        digits = "0." + std::string(static_cast<std::size_t>(-decimalAt), '0') +
                 digits;
    } else if (static_cast<std::size_t>(decimalAt) >= digits.size()) {
        digits.append(static_cast<std::size_t>(decimalAt) - digits.size(), '0');
    } else {
        digits.insert(static_cast<std::size_t>(decimalAt), 1, '.');
    }
    return negative ? "-" + digits : digits;
}

class Parser {
public:
    explicit Parser(std::string_view expression) : expression_(expression) {}

    CalculationResult run() {
        if (expression_.size() > kMaxExpressionBytes) {
            return {false, {}, "表达式过长"};
        }
        const long double value = parseExpression(0);
        skipSpaces();
        if (error_.empty() && position_ != expression_.size()) {
            fail("表达式含有无效字符或多余内容");
        }
        if (!error_.empty()) {
            return {false, {}, std::move(error_)};
        }
        return {true, formatResult(value), {}};
    }

private:
    void skipSpaces() {
        while (position_ < expression_.size() && isSpace(expression_[position_])) {
            ++position_;
        }
    }

    char peek() {
        skipSpaces();
        return position_ < expression_.size() ? expression_[position_] : '\0';
    }

    void fail(std::string_view message) {
        if (error_.empty()) {
            error_ = message;
        }
    }

    long double checkFinite(long double value) {
        if (!std::isfinite(value)) {
            fail("计算结果超出范围");
            return 0;
        }
        return value;
    }

    long double parseExpression(unsigned depth) {
        long double value = parseTerm(depth);
        while (error_.empty()) {
            const char op = peek();
            if (op != '+' && op != '-') {
                break;
            }
            ++position_;
            const long double rhs = parseTerm(depth);
            value = checkFinite(op == '+' ? value + rhs : value - rhs);
        }
        return value;
    }

    long double parseTerm(unsigned depth) {
        long double value = parseFactor(depth);
        while (error_.empty()) {
            const char op = peek();
            if (op != '*' && op != '/' && op != '%') {
                break;
            }
            ++position_;
            const long double rhs = parseFactor(depth);
            if (!error_.empty()) {
                return 0;
            }
            if ((op == '/' || op == '%') && rhs == 0) {
                fail("除数不能为零");
                return 0;
            }
            if (op == '*') {
                value *= rhs;
            } else if (op == '/') {
                value /= rhs;
            } else {
                value = std::fmod(value, rhs);
            }
            value = checkFinite(value);
        }
        return value;
    }

    long double parseFactor(unsigned depth) {
        bool negative = false;
        for (char sign = peek(); sign == '+' || sign == '-'; sign = peek()) {
            negative = negative != (sign == '-');
            ++position_;
        }

        long double value = 0;
        if (peek() == '(') {
            if (depth >= kMaxParenthesisDepth) {
                fail("括号嵌套过深");
                return 0;
            }
            ++position_;
            value = parseExpression(depth + 1);
            if (!error_.empty()) {
                return 0;
            }
            if (peek() != ')') {
                fail("缺少右括号");
                return 0;
            }
            ++position_;
        } else {
            value = parseNumber();
        }
        return negative ? -value : value;
    }

    long double parseNumber() {
        // Parse ASCII decimals directly; the process locale is never changed.
        skipSpaces();
        long double value = 0;
        bool hasDigits = false;
        while (position_ < expression_.size() && isDigit(expression_[position_])) {
            hasDigits = true;
            value = checkFinite(value * 10 + (expression_[position_++] - '0'));
            if (!error_.empty()) {
                return 0;
            }
        }
        if (position_ < expression_.size() && expression_[position_] == '.') {
            ++position_;
            long double place = 0.1L;
            while (position_ < expression_.size() &&
                   isDigit(expression_[position_])) {
                hasDigits = true;
                value += (expression_[position_++] - '0') * place;
                place /= 10;
            }
        }
        if (!hasDigits) {
            fail("需要数字或括号表达式");
        }
        return checkFinite(value);
    }

    std::string_view expression_;
    std::size_t position_ = 0;
    std::string error_;
};

} // namespace

CalculationResult calculate(std::string_view expression) {
    return Parser(expression).run();
}

} // namespace modernime::core
