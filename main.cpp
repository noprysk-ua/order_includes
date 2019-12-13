#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

static constexpr auto g_usage = R"(
order_includes sorts includes in go files
includes are divided into three groups: stdlib, platform and third parties
within the groups, they are sorted lexicographically

Usage:
./order_includes [file|directory]

Example:
./order_includes ../connection.go
./order_includes ../memsql/
)";

static constexpr auto g_deleted_line = "${THIS_LINE_IS_DELETED}";

struct Result {
    std::string m_path;
    std::string m_result_message;
};

enum class ModuleType { StdLib, Platform, ThirdParty, None };

using It = std::vector<std::string>::iterator;

std::vector<std::string> readFile(const fs::path& path) {
    std::vector<std::string> result;
    std::ifstream file{path};
    for (std::string line; std::getline(file, line);) {
        result.emplace_back(std::move(line));
    }
    return result;
}

std::string removeSpaces(const std::string& str) {
    std::string result;
    for (const auto& ch : str) {
        if (!std::isspace(ch)) {
            result += ch;
        }
    }
    return result;
}

std::string removeComments(const std::string& str) {
    const auto commentBegin = str.find("//");
    if (commentBegin == std::string::npos) {
        return str;
    }
    return std::string(str.begin(), str.begin() + commentBegin);
}

std::pair<It, It> findIncludes(std::vector<std::string>& lines) {
    auto begin = lines.begin();
    while (begin < lines.end()) {
        if (removeComments(removeSpaces(*begin)) == "import(") {
            ++begin;
            break;
        }
        ++begin;
    }
    auto end = begin;
    while (end < lines.end()) {
        if (removeComments(removeSpaces(*end)) == ")") {
            break;
        }
        ++end;
    }
    return {begin, end};
}

void deleteEmptyLines(It begin, It end) {
    while (begin < end) {
        if (begin->empty()) {
            *begin = g_deleted_line;
        }
        else if (std::all_of(begin->begin(), begin->end(),
                    [](const auto ch){return std::isspace(ch);})) {
            *begin = g_deleted_line;
        }
        ++begin;
    }
}

bool isThirdPartyModule(const std::string& str) {
    return str.find(R"("github.com/)") != std::string::npos ||
        str.find(R"("gopkg.in/)") != std::string::npos ||
        str.find(R"("golang.org/)") != std::string::npos ||
        str.find(R"("pault.ag/)") != std::string::npos;
}

bool isPlatformModule(const std::string& str) {
    return str.find(R"("platform/)") != std::string::npos;
}

bool isStdLibModule(const std::string& str) {
    if (str.empty()) {
        return false;
    }
    if (std::all_of(str.begin(), str.end(),
                [](const auto& ch){return std::isspace(ch);})) {
        return false;
    }
    if (str == g_deleted_line) {
        return false;
    }
    if (isThirdPartyModule(str)) {
        return false;
    }
    if (isPlatformModule(str)) {
        return false;
    }
    if (removeSpaces(str).find("//") == 0) {
        return false;
    }
    return true;
}

ModuleType moduleType(const std::string& str) {
    if (isThirdPartyModule(str)) {
        return ModuleType::ThirdParty;
    }
    if (isPlatformModule(str)) {
        return ModuleType::Platform;
    }
    if (isStdLibModule(str)) {
        return ModuleType::StdLib;
    }
    return ModuleType::None;
}

std::string removeUserModuleName(const std::string& str) {
    const auto quoteBegin = str.find(R"(")");
    if (quoteBegin == std::string::npos) {
        return str;
    }
    return std::string(str.begin() + quoteBegin, str.end());
}

const auto cmp = [](const auto& lhs, const auto& rhs) {
    if (lhs == g_deleted_line && rhs == g_deleted_line) {
        return false;
    }
    if (lhs == g_deleted_line) {
        return false;
    }
    if (rhs == g_deleted_line) {
        return true;
    }
    const auto lhsType = moduleType(lhs);
    const auto rhsType = moduleType(rhs);
    if (lhsType != rhsType) {
        return lhsType < rhsType;
    }
    const auto pureLhs = removeUserModuleName(removeSpaces(lhs));
    const auto pureRhs = removeUserModuleName(removeSpaces(rhs));
    return pureLhs < pureRhs;
};

bool withinIncludes(It i, It begin, It end) {
    return i >= begin && i < end;
}

void writeWithoutDeletedLinesAndWithSeparatedGroups(const fs::path& path,
                                std::vector<std::string>& lines) {
    const auto [includeBegin, includeEnd] = findIncludes(lines);
    std::ofstream file{path};
    for (auto i = 0; i < lines.size(); ++i) {
        if (lines[i] != g_deleted_line) {
            file << lines[i] << std::endl;
        }
        if (lines[i] != g_deleted_line && i != lines.size() - 1 &&
                lines[i + 1] != g_deleted_line &&
                withinIncludes(lines.begin() + i, includeBegin, includeEnd) &&
                withinIncludes(lines.begin() + i + 1, includeBegin, includeEnd)) {
            const auto moduleTypeCurrent = moduleType(lines[i]);
            const auto moduleTypeNext = moduleType(lines[i + 1]);
            if (moduleTypeCurrent != ModuleType::None  &&
                    moduleTypeNext != ModuleType::None &&
                    moduleTypeCurrent != moduleTypeNext) {
                file << std::endl;
            }
        }
    }
}

Result formatFile(const fs::path& path) {
    auto lines = readFile(path);
    if (lines.empty()) {
        return {path.u8string(), "failed to read from file"};
    }
    const auto [begin, end] = findIncludes(lines);
    if (begin >= end) {
        return {path.u8string(), "no includes found"};
    }
    deleteEmptyLines(begin, end);
    std::sort(begin, end, cmp);
    writeWithoutDeletedLinesAndWithSeparatedGroups(path, lines);
    return {path.u8string(), "done"};
}

} // anonymous namespace


int main(const int argc, const char** argv) {
    if (argc != 2) {
        std::cerr << g_usage << std::endl;
        return -1;
    }
    try {
        auto result = std::vector<Result>{};
        const auto path = fs::path(argv[1]);
        if (fs::is_directory(path)) {
                for(auto& p: fs::recursive_directory_iterator(path)) {
                    const auto file_path = fs::path(p);
                    if (file_path.extension().u8string() == u8".go") {
                        result.emplace_back(formatFile(p));
                    }
                }
        }
        else {
            if (path.extension().u8string() == u8".go") {
                result.emplace_back(formatFile(path));
            }
        }
        for (const auto& r : result) {
            std::cout << "[" << r.m_path << "][" <<
                r.m_result_message << "]" << std::endl;
        }
        if (result.empty()) {
            std::cerr << "no go files to order includes" << std::endl;
            return -3;
        }
    }
    catch (...) {
        std::cerr << "unexpected error occured" << std::endl;
        return -2;
    }
    return 0;
}
