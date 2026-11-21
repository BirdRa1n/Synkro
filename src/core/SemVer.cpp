#include "core/SemVer.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace synkro {

namespace {

bool numeric(const std::string& s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}

bool parseNumber(std::string_view s, int& out)
{
    if (s.empty() || (s.size() > 1 && s[0] == '0'))
        return false;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), out);
    return r.ec == std::errc() && r.ptr == s.data() + s.size();
}

} // namespace

SemVer parseSemVer(std::string_view text)
{
    SemVer v;
    if (!text.empty() && (text[0] == 'v' || text[0] == 'V'))
        text.remove_prefix(1);
    if (const auto plus = text.find('+'); plus != std::string_view::npos)
        text = text.substr(0, plus);
    std::string_view core = text, pre;
    if (const auto dash = text.find('-'); dash != std::string_view::npos) {
        core = text.substr(0, dash);
        pre = text.substr(dash + 1);
        if (pre.empty())
            return v;
    }
    int* parts[] = {&v.major, &v.minor, &v.patch};
    for (int i = 0; i < 3; ++i) {
        const auto dot = core.find('.');
        if ((i < 2) == (dot == std::string_view::npos))
            return v;
        if (!parseNumber(core.substr(0, dot), *parts[i]))
            return v;
        core = dot == std::string_view::npos ? std::string_view() : core.substr(dot + 1);
    }
    while (!pre.empty()) {
        const auto dot = pre.find('.');
        std::string id(pre.substr(0, dot));
        if (id.empty() || !std::all_of(id.begin(), id.end(), [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '-';
            }))
            return v;
        v.prerelease.push_back(id);
        if (dot == std::string_view::npos)
            break;
        pre = pre.substr(dot + 1);
        if (pre.empty())
            return v;
    }
    v.valid = true;
    return v;
}

int compareSemVer(const SemVer& a, const SemVer& b)
{
    if (a.major != b.major)
        return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor)
        return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch)
        return a.patch < b.patch ? -1 : 1;
    if (a.prerelease.empty() || b.prerelease.empty()) {
        if (a.prerelease.empty() && b.prerelease.empty())
            return 0;
        return a.prerelease.empty() ? 1 : -1; // final > pré-lançamento
    }
    for (std::size_t i = 0; i < std::min(a.prerelease.size(), b.prerelease.size()); ++i) {
        const std::string& x = a.prerelease[i];
        const std::string& y = b.prerelease[i];
        const bool nx = numeric(x), ny = numeric(y);
        if (nx && ny) {
            if (x.size() != y.size())
                return x.size() < y.size() ? -1 : 1;
            if (x != y)
                return x < y ? -1 : 1;
        } else if (nx != ny) {
            return nx ? -1 : 1; // numérico < alfanumérico
        } else if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    if (a.prerelease.size() != b.prerelease.size())
        return a.prerelease.size() < b.prerelease.size() ? -1 : 1;
    return 0;
}

bool isNewerVersion(std::string_view candidate, std::string_view current)
{
    const SemVer c = parseSemVer(candidate), cur = parseSemVer(current);
    return c.valid && cur.valid && compareSemVer(c, cur) > 0;
}

} // namespace synkro
