// This lexical guard checks explicit source spellings after removing comments and literals. It
// exempts adapter/drag function locals and foreign signatures, plus members of classes there whose
// names end in Handle, Window or Guard. Explicit auto*/decltype* pointers remain visible, but the
// scan cannot infer ownership from auto/decltype without a pointer spelling, aliases outside its
// named Win32-handle list, macros or data flow; clang-tidy, cppcheck and review cover those gaps.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{

    struct Violation
    {
        std::string token;
    };

    struct ClassScope
    {
        std::string name;
        std::size_t open{};
        std::size_t close{};
    };

    std::string readFile(const std::filesystem::path &path)
    {
        std::ifstream input{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    std::string codeOnly(std::string_view source)
    {
        std::string result{source};
        static const std::regex blockComments{R"(/\*[\s\S]*?\*/)"};
        static const std::regex lineComments{R"(//[^\r\n]*)"};
        static const std::regex stringLiterals{R"lit("(?:\\.|[^"\\])*")lit"};
        static const std::regex characterLiterals{R"lit('(?:\\.|[^'\\])*')lit"};
        result = std::regex_replace(result, blockComments, " ");
        result = std::regex_replace(result, lineComments, " ");
        result = std::regex_replace(result, stringLiterals, " ");
        return std::regex_replace(result, characterLiterals, " ");
    }

    std::string withoutComments(std::string_view source)
    {
        std::string result{source};
        static const std::regex blockComments{R"(/\*[\s\S]*?\*/)"};
        static const std::regex lineComments{R"(//[^\r\n]*)"};
        result = std::regex_replace(result, blockComments, " ");
        return std::regex_replace(result, lineComments, " ");
    }

    bool containsInclude(std::string_view source, std::string_view header)
    {
        const std::regex include{"#\\s*include\\s*[<\"]" + std::string{header} + "[>\"]", std::regex::icase};
        return std::regex_search(source.begin(), source.end(), include);
    }

    std::vector<ClassScope> classScopes(const std::string &code)
    {
        std::vector<ClassScope> scopes;
        static const std::regex declaration{R"(\b(?:class|struct)\s+([A-Za-z_][A-Za-z0-9_:]*)[^;{]*\{)"};
        for (auto match = std::sregex_iterator{code.begin(), code.end(), declaration}; match != std::sregex_iterator{};
             ++match) {
            const auto open = code.find('{', static_cast<std::size_t>(match->position()));
            std::size_t depth{};
            for (auto position = open; position < code.size(); ++position) {
                if (code[position] == '{') {
                    ++depth;
                } else if (code[position] == '}' && --depth == 0) {
                    scopes.push_back({(*match)[1].str(), open, position});
                    break;
                }
            }
        }
        return scopes;
    }

    std::vector<ClassScope> functionScopes(const std::string &code)
    {
        std::vector<ClassScope> scopes;
        static const std::regex definition{R"(\b([~A-Za-z_][A-Za-z0-9_:~]*)\s*\([^;{}]*\)[^{;]*\{)"};
        for (auto match = std::sregex_iterator{code.begin(), code.end(), definition}; match != std::sregex_iterator{};
             ++match) {
            const auto name = (*match)[1].str();
            if (name == "if" || name == "for" || name == "while" || name == "switch" || name == "catch") {
                continue;
            }
            const auto open = code.find('{', static_cast<std::size_t>(match->position()));
            std::size_t depth{};
            for (auto position = open; position < code.size(); ++position) {
                if (code[position] == '{') {
                    ++depth;
                } else if (code[position] == '}' && --depth == 0) {
                    scopes.push_back({(*match)[1].str(), open, position});
                    break;
                }
            }
        }
        return scopes;
    }

    const ClassScope *innermostClass(std::size_t position, const std::vector<ClassScope> &scopes)
    {
        const ClassScope *owner{};
        for (const auto &scope : scopes) {
            if (position > scope.open && position < scope.close &&
                (owner == nullptr || scope.close - scope.open < owner->close - owner->open)) {
                owner = &scope;
            }
        }
        return owner;
    }

    bool directlyInsideClass(std::size_t position, const std::string &code, const ClassScope &scope)
    {
        std::size_t depth{};
        for (auto index = scope.open + 1; index < position; ++index) {
            if (code[index] == '{') {
                ++depth;
            } else if (code[index] == '}') {
                --depth;
            }
        }
        return depth == 0;
    }

    bool insideScope(std::size_t position, const std::vector<ClassScope> &scopes)
    {
        for (const auto &scope : scopes) {
            if (position > scope.open && position < scope.close) {
                return true;
            }
        }
        return false;
    }

    bool insideParentheses(std::size_t position, const std::string &code)
    {
        std::size_t depth{};
        for (std::size_t index = 0; index < position; ++index) {
            if (code[index] == '(') {
                ++depth;
            } else if (code[index] == ')' && depth != 0) {
                --depth;
            }
        }
        return depth != 0;
    }

    bool wrapperName(std::string_view name)
    {
        return name.ends_with("Handle") || name.ends_with("Window") || name.ends_with("Guard");
    }

    bool allowedForeignDeclaration(std::size_t position, const std::string &code,
                                   const std::vector<ClassScope> &classes, const std::vector<ClassScope> &functions)
    {
        if (insideParentheses(position, code)) {
            return true;
        }
        const auto owner = innermostClass(position, classes);
        if (owner != nullptr && directlyInsideClass(position, code, *owner)) {
            return wrapperName(owner->name);
        }
        return insideScope(position, functions);
    }

    std::vector<Violation> scan(std::string_view relativePath, std::string_view source)
    {
        std::vector<Violation> violations;
        const std::string normalized = std::filesystem::path{relativePath}.generic_string();
        const bool core = normalized.starts_with("src/core/");
        const bool farAdapter = normalized.starts_with("src/adapters/far/");
        const bool adapter = normalized.starts_with("src/adapters/");
        const bool drag = normalized.starts_with("src/drag/");
        const bool plugin = normalized.starts_with("src/plugin/");
        const bool exports = normalized == "src/plugin/Exports.cpp";
        const std::string includes = withoutComments(source);

        if (core) {
            for (const std::string_view header : {"windows\\.h", "shlobj\\.h", "shobjidl\\.h", "plugin\\.hpp"}) {
                if (containsInclude(includes, header)) {
                    violations.push_back({std::string{header}});
                }
            }
        }
        if (containsInclude(includes, "plugin\\.hpp") && !farAdapter && !plugin) {
            violations.push_back({"plugin.hpp"});
        }
        if (drag && containsInclude(includes, "adapters/[^>\"]+")) {
            violations.push_back({"drag -> adapters"});
        }

        const std::string code = codeOnly(source);
        static const std::regex ownership{R"(\b(new|delete|malloc|free|shared_ptr|weak_ptr)\b)"};
        for (auto match = std::sregex_iterator{code.begin(), code.end(), ownership}; match != std::sregex_iterator{};
             ++match) {
            violations.push_back({match->str()});
        }
        const auto scopes = classScopes(code);
        const auto functions = functionScopes(code);
        const bool foreignArea = adapter || drag;
        static const std::regex rawPointer{
            R"((?:^|[\n;{},(])([ \t]*(?:(?:public|private|protected)[ \t]*:[ \t]*)?(?:(?:const|volatile|mutable|constexpr|static|inline|extern|thread_local|signed|unsigned|short|long|typename|class|struct|union|enum)\s+)*(?:auto|decltype\s*\([^;\n{}]*\)|[A-Za-z_][A-Za-z0-9_:]*(?:<[^;\n{}()]*>)?)(?:\s+const)?[ \t]*\*+[ \t]*(?:(?:const|volatile)[ \t]+)*[A-Za-z_][A-Za-z0-9_]*[ \t]*(?=[=;{},)\[])))"};
        for (auto match = std::sregex_iterator{code.begin(), code.end(), rawPointer}; match != std::sregex_iterator{};
             ++match) {
            const auto position = static_cast<std::size_t>(match->position(1));
            if (!exports && !(foreignArea && allowedForeignDeclaration(position, code, scopes, functions))) {
                violations.push_back({"raw pointer declaration"});
            }
        }
        static const std::regex win32Handle{
            R"((?:^|[\n;{},(])([ \t]*(?:(?:public|private|protected)[ \t]*:[ \t]*)?(?:(?:const|volatile|mutable|constexpr|static|inline|extern|thread_local)\s+)*(?:HANDLE|HWND|HMODULE|HDC|HICON|HMENU|HGLOBAL|HKEY|HHOOK)[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*(?=[=;{},)\[])))"};
        for (auto match = std::sregex_iterator{code.begin(), code.end(), win32Handle}; match != std::sregex_iterator{};
             ++match) {
            const auto position = static_cast<std::size_t>(match->position(1));
            if (!exports && !(foreignArea && allowedForeignDeclaration(position, code, scopes, functions))) {
                violations.push_back({"Win32 handle declaration"});
            }
        }
        if (code.find("__try") != std::string::npos && normalized != "src/adapters/far/PluginCall.cpp") {
            violations.push_back({"__try"});
        }
        return violations;
    }

} // namespace

TEST_SUITE("source guard")
{
    TEST_CASE("scanner rejects forbidden boundaries and ignores comments and literals")
    {
        CHECK_FALSE(scan("src/core/Bad.cpp", "#include <windows.h>").empty());
        CHECK_FALSE(scan("src/drag/Bad.cpp", "#include <plugin.hpp>").empty());
        CHECK_FALSE(scan("src/drag/Bad.cpp", "#include \"adapters/shell/Shell.hpp\"").empty());
        CHECK_FALSE(scan("src/core/Bad.cpp", "auto p = new Thing;").empty());
        CHECK_FALSE(scan("src/plugin/Bad.cpp", "__try {}").empty());
        CHECK_FALSE(scan("src/plugin/Composition.hpp", "struct Bad { PluginStartupInfo *startup_; };").empty());
        CHECK_FALSE(scan("src/core/Bad.hpp", "struct Bad { int *owner_; };").empty());
        CHECK_FALSE(scan("src/core/Bad.hpp", "struct Bad { Widget* owner_; };").empty());
        CHECK_FALSE(scan("src/core/Bad.hpp", "struct Bad { auto *owner_; };").empty());
        CHECK_FALSE(scan("src/core/Bad.hpp", "struct Bad { struct Widget *owner_; };").empty());
        CHECK_FALSE(scan("src/core/Bad.hpp", "struct Bad { decltype(value) *owner_; };").empty());
        CHECK_FALSE(scan("src/core/Bad.hpp", "struct Bad { Widget *const owner_{}; };").empty());
        CHECK_FALSE(scan("src/core/Bad.hpp", "struct Bad { void visit() {} int *owner_; };").empty());
        CHECK_FALSE(scan("src/core/Bad.hpp", "class Bad { private: int *owner_; };").empty());
        CHECK_FALSE(scan("src/core/Bad.hpp", "struct Bad { int value; auto* owner_; };").empty());
        CHECK_FALSE(scan("src/drag/Bad.hpp", "class Bad { IDataObject *data_; };").empty());
        CHECK_FALSE(scan("src/drag/Bad.cpp", "IDataObject *g_data;").empty());
        CHECK_FALSE(scan("src/adapters/shell/Bad.hpp", "struct ForeignCalls { IDataObject *data_; };").empty());
        CHECK_FALSE(scan("src/adapters/shell/Bad.cpp", "IDataObject *g_data;").empty());
        CHECK_FALSE(
            scan("src/adapters/shell/Bad.cpp", "void call() { struct Local { IDataObject *data_; }; }").empty());
        CHECK(
            scan("src/adapters/shell/Good.cpp", "void call() { IDataObject *data{}; foreign(&data); data->Release(); }")
                .empty());
        CHECK(scan("src/drag/Good.hpp", "HRESULT call(IDataObject *data, DWORD *effect);").empty());
        CHECK(scan("src/adapters/shell/Good.hpp", "class DataHandle { IDataObject *value_; };").empty());
        CHECK_FALSE(scan("src/core/Bad.hpp", "class DataHandle { Thing *value_; };").empty());
        CHECK_FALSE(scan("src/drag/Bad.cpp", "HWND window = CreateWindowExW();").empty());
        CHECK_FALSE(scan("src/drag/Bad.cpp", "HANDLE event{CreateEventW()};").empty());
        CHECK_FALSE(scan("src/drag/Bad.cpp", "HMODULE module = LoadLibraryW();").empty());
        CHECK_FALSE(scan("src/drag/Bad.cpp", "HDC dc{CreateCompatibleDC()};").empty());
        CHECK_FALSE(scan("src/plugin/Bad.cpp", "HICON icon{};").empty());
        CHECK_FALSE(scan("src/plugin/Bad.cpp", "HMENU menu{};").empty());
        CHECK_FALSE(scan("src/plugin/Bad.cpp", "HGLOBAL memory{};").empty());
        CHECK_FALSE(scan("src/plugin/Bad.cpp", "HKEY key{};").empty());
        CHECK_FALSE(scan("src/plugin/Bad.cpp", "HHOOK hook{};").empty());
        CHECK_FALSE(
            scan("src/drag/Bad.cpp", "HANDLE g_thread = nullptr; void start() { g_thread = CreateThread(); }").empty());
        CHECK_FALSE(scan("src/drag/Bad.cpp", "void call() { struct Local { HANDLE value{}; }; }").empty());
        CHECK(scan("src/drag/Good.cpp", "void call() { HANDLE event{}; event = CreateEventW(); CloseHandle(event); }")
                  .empty());
        CHECK(scan("src/drag/Good.cpp", "class ToolWindow { HWND value{CreateWindowExW()}; };").empty());
        CHECK(scan("src/drag/Good.cpp", "class EventHandle { HANDLE value = CreateEventW(); };").empty());
        CHECK(scan("src/drag/Good.cpp", "void EventHandle::reset() { HANDLE value = CreateEventW(); }").empty());
        CHECK(scan("src/core/Good.cpp", "// new and #include <windows.h>\nauto text = \"delete\";").empty());
        CHECK(scan("src/adapters/far/PluginCall.cpp", "#include <plugin.hpp>\n__try {}").empty());
        CHECK(scan("src/drag/Good.hpp", "HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **object) override;")
                  .empty());
    }

    TEST_CASE("the complete source tree obeys layering, ownership, and the single-SEH rule")
    {
        const auto repository = std::filesystem::path{BURLAK_SOURCE_DIR};
        std::ostringstream failures;
        std::size_t scanned{};
        std::size_t count{};
        std::size_t sehCount{};
        for (const auto &entry : std::filesystem::recursive_directory_iterator{repository / "src"}) {
            if (!entry.is_regular_file() || (entry.path().extension() != ".cpp" && entry.path().extension() != ".hpp" &&
                                             entry.path().extension() != ".h")) {
                continue;
            }
            ++scanned;
            const auto relative = std::filesystem::relative(entry.path(), repository).generic_string();
            const auto source = readFile(entry.path());
            if (codeOnly(source).find("__try") != std::string::npos) {
                ++sehCount;
            }
            for (const auto &violation : scan(relative, source)) {
                ++count;
                failures << relative << ": forbidden token " << violation.token << '\n';
            }
        }
        INFO(failures.str());
        CHECK(scanned > 0);
        CHECK(count == 0);
        CHECK(sehCount == 1);
    }
}
