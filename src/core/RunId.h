#pragma once

#include <QDateTime>

#include <string>

namespace synkro {

// Identificador de execução ordenável por data: r-AAAAMMDD-HHMMSS-xxxx (UTC).
// A ordenação lexicográfica dos IDs coincide com a ordem cronológica, o que
// permite listar versions\ e runs\ sem abrir arquivos.
std::wstring newRunId(const QDateTime& utcNow = QDateTime::currentDateTimeUtc());

// Data/hora UTC codificada no ID (inválida se o formato não for reconhecido).
QDateTime runIdTimestamp(const std::wstring& runId);

bool isValidRunId(const std::wstring& runId);

// IDs de tarefa e de cofre na configuração: [a-z0-9-], 1..64, sem hífen nas pontas.
bool isValidJobId(const std::wstring& id);

} // namespace synkro
