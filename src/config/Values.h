#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace synkro::config {

// Conversões das convenções de docs/configuration.md §4.

// "30s", "10m", "2h", "90d" -> segundos.
std::optional<std::int64_t> parseDuration(const std::string& text);

// "500MB", "2GB", "1,5 TB" -> bytes (base 1024).
std::optional<std::uint64_t> parseSize(const std::string& text);

// "20%" -> 0.20. Aceita também número puro entre 0 e 1 ("0.5").
std::optional<double> parseRatio(const std::string& text);

// "22:00" -> minutos desde a meia-noite.
std::optional<int> parseTimeOfDay(const std::string& text);

// "mon".."sun" -> 1..7 (segunda = 1).
std::optional<int> parseWeekday(const std::string& text);

// Descrições em pt-BR.
std::string describeDuration(std::int64_t seconds);
std::string describeTime(int minutes);
std::string describeWeekdays(const std::vector<int>& days);
std::string describePercent(double ratio);
std::string describeBytes(std::uint64_t bytes);

} // namespace synkro::config
