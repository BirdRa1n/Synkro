#include "scan/PathFilter.h"

#include "platform/windows/Win32.h"

namespace synkro {

namespace {

enum class Tok { Char, Any, Star, StarStar, StarStarSlash };

struct Token {
    Tok kind;
    wchar_t ch = 0;
};

std::vector<Token> tokenize(std::wstring_view p)
{
    std::vector<Token> t;
    for (size_t i = 0; i < p.size(); ++i) {
        const wchar_t c = p[i];
        if (c == L'*') {
            if (i + 1 < p.size() && p[i + 1] == L'*') {
                if (i + 2 < p.size() && p[i + 2] == L'/') {
                    t.push_back({Tok::StarStarSlash});
                    i += 2;
                } else {
                    t.push_back({Tok::StarStar});
                    i += 1;
                }
            } else {
                t.push_back({Tok::Star});
            }
        } else if (c == L'?') {
            t.push_back({Tok::Any});
        } else {
            t.push_back({Tok::Char, c});
        }
    }
    return t;
}

} // namespace

bool matchGlob(std::wstring_view patternIn, std::wstring_view pathIn)
{
    const std::wstring pattern = win::foldCase(patternIn);
    const std::wstring path = win::foldCase(pathIn);
    const auto toks = tokenize(pattern);
    const size_t n = toks.size();
    const size_t m = path.size();

    // dp[i][j]: tokens [0,i) casam com path [0,j)
    std::vector<std::vector<char>> dp(n + 1, std::vector<char>(m + 1, 0));
    dp[0][0] = 1;
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j <= m; ++j) {
            if (!dp[i][j])
                continue;
            const Token& tk = toks[i];
            switch (tk.kind) {
            case Tok::Char:
                if (j < m && path[j] == tk.ch)
                    dp[i + 1][j + 1] = 1;
                break;
            case Tok::Any:
                if (j < m && path[j] != L'/')
                    dp[i + 1][j + 1] = 1;
                break;
            case Tok::Star:
                dp[i + 1][j] = 1;
                for (size_t k = j; k < m && path[k] != L'/'; ++k)
                    dp[i + 1][k + 1] = 1;
                break;
            case Tok::StarStar:
                for (size_t k = j; k <= m; ++k)
                    dp[i + 1][k] = 1;
                break;
            case Tok::StarStarSlash:
                dp[i + 1][j] = 1;
                for (size_t k = j; k < m; ++k) {
                    if (path[k] == L'/')
                        dp[i + 1][k + 1] = 1;
                }
                break;
            }
        }
    }
    return dp[n][m] != 0;
}

std::wstring PathFilter::normalize(const std::wstring& patternIn)
{
    std::wstring p = win::toCatalogPath(patternIn); // '\' -> '/'
    if (!patternIn.empty() && (patternIn.front() == L'/' || patternIn.front() == L'\\'))
        return p; // ancorado: toCatalogPath já removeu a barra inicial
    std::wstring body = p;
    while (!body.empty() && body.back() == L'/')
        body.pop_back();
    if (body.find(L'/') == std::wstring::npos)
        return L"**/" + p;
    return p;
}

PathFilter::PathFilter(std::vector<std::wstring> include, std::vector<std::wstring> exclude)
{
    for (auto& p : include) {
        if (!p.empty())
            m_include.push_back(normalize(p));
    }
    for (auto& p : exclude) {
        if (!p.empty())
            m_exclude.push_back(normalize(p));
    }
}

bool PathFilter::accepts(std::wstring_view path, bool isDirectory) const
{
    if (isDirectory) {
        const std::wstring withSlash = std::wstring(path) + L'/';
        for (const auto& p : m_exclude) {
            if (matchGlob(p, path) || matchGlob(p, withSlash))
                return false;
        }
        return true;
    }

    for (const auto& p : m_exclude) {
        if (matchGlob(p, path))
            return false;
    }
    if (m_include.empty())
        return true;
    for (const auto& p : m_include) {
        if (matchGlob(p, path))
            return true;
    }
    return false;
}

} // namespace synkro
