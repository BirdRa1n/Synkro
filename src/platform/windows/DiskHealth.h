#pragma once

#include "core/Result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace synkro::win {

enum class HealthStatus { Healthy, Warning, Unhealthy, Unknown };

const char* nameOf(HealthStatus s) noexcept;

// Saúde de um disco físico segundo o Windows (WMI, namespace
// root\Microsoft\Windows\Storage — a mesma fonte do "Gerenciador de
// Armazenamento"). Os contadores de confiabilidade (temperatura, desgaste,
// erros de leitura) só vêm quando o processo é administrador.
struct DiskHealth {
    std::uint32_t diskNumber = 0;
    std::string model;
    std::string busType;          // USB, SATA, NVMe...
    std::string mediaType;        // HDD, SSD
    HealthStatus status = HealthStatus::Unknown;
    std::vector<std::string> problems; // pt-BR, a partir de OperationalStatus
    std::optional<int> temperatureC;
    std::optional<int> wearPercent;    // SSD: % de vida consumida
    std::optional<std::uint64_t> readErrorsUncorrected;
};

Result<std::vector<DiskHealth>> queryDiskHealth();

// Saúde do disco físico de número `n` (nullopt se o WMI não o conhecer).
std::optional<DiskHealth> healthOfDisk(std::uint32_t n);

// Resumo de uma linha em pt-BR ("Saudável · SSD NVMe · 38 °C").
std::string describeHealth(const DiskHealth& h);

} // namespace synkro::win
