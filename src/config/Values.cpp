#include "config/Values.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <regex>

namespace synkro::config {

namespace {

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

std::optional<std::int64_t> parseDuration(const std::string& text)
{
    static const std::regex re(R"(^\s*(\d+)\s*([smhd])\s*$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(text, m, re))
        return std::nullopt;
    const std::int64_t n = std::stoll(m[1].str());
    switch (std::tolower(static_cast<unsigned char>(m[2].str()[0]))) {
    case 's': return n;
    case 'm': return n * 60;
    case 'h': return n * 3600;
    case 'd': return n * 86400;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> parseSize(const std::string& text)
{
    static const std::regex re(R"(^\s*(\d+(?:[.,]\d+)?)\s*(KB|MB|GB|TB)\s*$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(text, m, re))
        return std::nullopt;
    std::string num = m[1].str();
    std::replace(num.begin(), num.end(), ',', '.');
    const double v = std::stod(num);
    const std::string unit = lower(m[2].str());
    double mult = 1024.0;
    if (unit == "mb") mult = 1024.0 * 1024;
    else if (unit == "gb") mult = 1024.0 * 1024 * 1024;
    else if (unit == "tb") mult = 1024.0 * 1024 * 1024 * 1024;
    return static_cast<std::uint64_t>(std::llround(v * mult));
}

std::optional<double> parseRatio(const std::string& textIn)
{
    const std::string text = trim(textIn);
    if (text.empty())
        return std::nullopt;
    try {
        size_t used = 0;
        if (text.back() == '%') {
            const double v = std::stod(text.substr(0, text.size() - 1), &used);
            if (used != text.size() - 1 || v < 0 || v > 100)
                return std::nullopt;
            return v / 100.0;
        }
        const double v = std::stod(text, &used);
        if (used != text.size() || v < 0 || v > 1)
            return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<int> parseTimeOfDay(const std::string& text)
{
    static const std::regex re(R"(^\s*([01]?\d|2[0-3]):([0-5]\d)\s*$)");
    std::smatch m;
    if (!std::regex_match(text, m, re))
        return std::nullopt;
    return std::stoi(m[1].str()) * 60 + std::stoi(m[2].str());
}

std::optional<int> parseWeekday(const std::string& text)
{
    static const char* names[] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
    const std::string t = lower(trim(text));
    for (int i = 0; i < 7; ++i) {
        if (t == names[i])
            return i + 1;
    }
    return std::nullopt;
}

std::string describeDuration(std::int64_t s)
{
    auto plural = [](std::int64_t n, const char* one, const char* many) {
        return std::to_string(n) + " " + (n == 1 ? one : many);
    };
    if (s % 86400 == 0 && s >= 86400) return plural(s / 86400, "dia", "dias");
    if (s % 3600 == 0 && s >= 3600) return plural(s / 3600, "hora", "horas");
    if (s % 60 == 0 && s >= 60) return plural(s / 60, "minuto", "minutos");
    return plural(s, "segundo", "segundos");
}

std::string describeTime(int minutes)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", minutes / 60, minutes % 60);
    return buf;
}

std::string describeWeekdays(const std::vector<int>& days)
{
    static const char* names[] = {"segunda", "terça", "quarta", "quinta", "sexta", "sábado", "domingo"};
    std::vector<int> d = days;
    std::sort(d.begin(), d.end());
    d.erase(std::unique(d.begin(), d.end()), d.end());
    if (d.size() == 7)
        return "todos os dias";
    if (d == std::vector<int>{1, 2, 3, 4, 5})
        return "de segunda a sexta";
    if (d == std::vector<int>{1, 2, 3, 4, 5, 6})
        return "de segunda a sábado";
    if (d == std::vector<int>{6, 7})
        return "sábados e domingos";
    std::string out;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i > 0)
            out += (i + 1 == d.size()) ? " e " : ", ";
        out += names[d[i] - 1];
    }
    return out;
}

std::string describePercent(double ratio)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.0f%%", ratio * 100.0);
    return buf;
}

std::string describeBytes(std::uint64_t bytes)
{
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
    std::string s = buf;
    std::replace(s.begin(), s.end(), '.', ',');
    return s;
}

} // namespace synkro::config
