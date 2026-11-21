#pragma once

#include "core/Result.h"

#include <string>

namespace synkro::win {

// Caminho UNC (\\servidor\compartilhamento\... ou //servidor/...). Não conta
// caminhos de dispositivo (\\?\, \\.\).
bool isUncPath(const std::wstring& path);

// Barras invertidas e sem barra no fim (exceto nada: "\\srv\share").
std::wstring normalizeUnc(const std::wstring& path);
std::wstring uncServer(const std::wstring& path);    // "srv"
std::wstring uncShareRoot(const std::wstring& path); // "\\srv\share"

// O servidor é esta própria máquina? (localhost, nome NetBIOS/DNS, um IP de
// alguma placa de rede.) Cofre num compartilhamento daqui não é redundância.
bool isThisComputer(const std::wstring& server);

// O servidor responde na porta do SMB (445)? Evita que um NAS desligado
// prenda o processo por ~30 s no redirecionador de rede. Resultados negativos
// ficam guardados por 60 s.
Status probeSmbServer(const std::wstring& server, int timeoutMs = -1);

// Tempo padrão da sondagem neste processo (3 s). O desktop usa menos: ele
// avalia os cofres na thread da interface.
void setSmbProbeTimeout(int timeoutMs);

// Abre uma conexão sem letra de unidade com o compartilhamento, com usuário
// e senha (o worker roda como SYSTEM, que não tem as unidades mapeadas nem as
// credenciais do usuário). A conexão vale para a sessão de logon do processo;
// chamar de novo é barato. Usuário vazio: nada a fazer (vale a conta do
// computador ou a sessão atual).
//
// `replaceConflicting`: se já houver conexão com o servidor usando outro
// usuário (ERROR_SESSION_CREDENTIAL_CONFLICT), desfaz e refaz. O worker usa
// (sessão do SYSTEM); o desktop não, para não derrubar uma unidade mapeada
// pelo usuário: devolve AlreadyExists e quem chamou tenta com a conexão atual.
Status connectShare(const std::wstring& uncPath, const std::wstring& user, const std::string& passwordUtf8,
                    bool replaceConflicting = true);

// Política do processo: false impede connectShare de desfazer conexões
// existentes, mesmo com replaceConflicting = true (o desktop chama no início).
void setReplaceConflictingConnections(bool allow);

} // namespace synkro::win
