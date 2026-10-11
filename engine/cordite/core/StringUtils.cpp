#include "cordite/core/StringUtils.h"

#include "cordite/core/Assert.h"

#include <algorithm>

namespace cordite {
namespace {

bool isAsciiSpace(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r' || character == '\f' ||
           character == '\v';
}

char toLowerAsciiChar(char character) {
    return (character >= 'A' && character <= 'Z') ? static_cast<char>(character - 'A' + 'a') : character;
}

} // namespace

std::string_view trim(std::string_view text) {
    size_t begin = 0;
    while (begin < text.size() && isAsciiSpace(text[begin])) {
        ++begin;
    }
    size_t end = text.size();
    while (end > begin && isAsciiSpace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::vector<std::string_view> split(std::string_view text, char delimiter) {
    std::vector<std::string_view> parts;
    size_t begin = 0;
    while (true) {
        const size_t end = text.find(delimiter, begin);
        if (end == std::string_view::npos) {
            parts.push_back(text.substr(begin));
            return parts;
        }
        parts.push_back(text.substr(begin, end - begin));
        begin = end + 1;
    }
}

std::string toLowerAscii(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), toLowerAsciiChar);
    return result;
}

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char left, char right) {
               return toLowerAsciiChar(left) == toLowerAsciiChar(right);
           });
}

std::string replaceAll(std::string_view text, std::string_view from, std::string_view to) {
    CORDITE_ASSERT(!from.empty(), "replaceAll: 'from' must not be empty");
    if (from.empty()) {
        return std::string(text);
    }

    std::string result;
    result.reserve(text.size());
    size_t begin = 0;
    while (true) {
        const size_t match = text.find(from, begin);
        if (match == std::string_view::npos) {
            result.append(text.substr(begin));
            return result;
        }
        result.append(text.substr(begin, match - begin));
        result.append(to);
        begin = match + from.size();
    }
}

} // namespace cordite
