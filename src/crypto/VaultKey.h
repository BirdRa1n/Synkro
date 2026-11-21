#pragma once

// Chave de um cofre cifrado (architecture.md §3.5).
//
// - Cada cofre cifrado tem uma chave aleatória de 256 bits.
// - No próprio cofre (SynkroVault\key.json, com uma segunda cópia em
//   key.backup.json) ela fica cifrada com a CHAVE DE
//   RECUPERAÇÃO: 160 bits aleatórios mostrados uma única vez ao usuário
//   (XXXX-XXXX-...). Com ela, qualquer computador restaura o cofre — inclusive
//   depois da perda do PDV.
// - Nesta máquina, uma cópia fica no cofre de segredos (DPAPI, nome
//   "vault-key.<vault_id>"), para o worker agendado cifrar sem ninguém digitar.
// - Um processo pode ainda guardar a chave só em memória (restauração no
//   desktop, num computador novo).

#include "crypto/FileCipher.h"

#include <QString>

#include <array>
#include <memory>
#include <string>

namespace synkro::crypto {

using RecoveryBytes = std::array<std::uint8_t, 20>;

std::string formatRecoveryKey(const RecoveryBytes& bytes);      // "ABCD-EFGH-..." (8 grupos)
Result<RecoveryBytes> parseRecoveryKey(std::string_view text);  // tolera minúsculas, espaços e 0/1/8

std::wstring keyFilePath(const std::wstring& vaultLocation);
bool isEncrypted(const std::wstring& vaultLocation);
std::string secretNameFor(const QString& vaultId);

struct EnableResult {
    std::string recoveryKey;     // mostrar UMA vez; não fica gravada em lugar nenhum
    bool storedOnMachine = false;
    std::string storeError;      // por que não ficou nesta máquina (ex.: sem administrador)
};

// Cifra um cofre recém-criado (sem tarefas): gera a chave, grava key.json e
// tenta guardar a chave nesta máquina. Cofres que já têm dados não podem ser
// cifrados (seriam metade cifrados): crie um cofre novo.
Result<EnableResult> enableEncryption(const std::wstring& vaultLocation, const std::wstring& secretsFile = {});

// Abre a chave do cofre com a chave de recuperação.
Result<Key> unlockWithRecoveryKey(const std::wstring& vaultLocation, std::string_view recoveryKey);

// Guarda a chave para este processo (sem gravar nada).
void rememberKey(const QString& vaultId, const Key& key);
// Esquece as chaves guardadas em memória (apaga os bytes).
void forgetRememberedKeys();
// Guarda a chave nesta máquina (DPAPI; requer acesso ao secrets.dat).
Status storeKeyOnMachine(const QString& vaultId, const Key& key, const std::wstring& secretsFile = {});

// Cifrador do cofre: nullptr se o cofre não é cifrado; KeyRequired se é e a
// chave não está disponível (nem na memória do processo, nem nesta máquina).
// Recusa gravar em claro num cofre que esta máquina sabe ser cifrado e cujo
// key.json sumiu.
Result<std::shared_ptr<const FileCipher>> cipherFor(const std::wstring& vaultLocation, const std::wstring& secretsFile = {});

} // namespace synkro::crypto
