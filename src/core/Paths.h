#pragma once

#include <string>

namespace synkro {

// Pasta de dados do Synkro: %SYNKRO_DATA_DIR% ou C:\ProgramData\Synkro.
// Contém config\, secrets.dat, state\synkro.db, staging\ e logs\.
std::wstring dataDirectory();

std::wstring stagingDirectory();   // <dados>\staging
std::wstring secretsFile();        // <dados>\secrets.dat
std::wstring stateDatabaseFile();  // <dados>\state\synkro.db

} // namespace synkro
