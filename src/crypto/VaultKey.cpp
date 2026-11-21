#include "crypto/VaultKey.h"

#include "core/Paths.h"
#include "engine/VaultOps.h"
#include "platform/windows/Win32.h"
#include "secrets/SecretStore.h"
#include "vault/Vault.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <cctype>
#include <cstring>
#include <map>
#include <mutex>

namespace synkro::crypto {

namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"; // RFC 4648
constexpr std::uint64_t kIterations = 200000; // a chave já tem 160 bits; o PBKDF2 é só reforço
constexpr char kKeyFormat[] = "synkro-vault-key";

std::mutex g_ringMutex;
std::map<QString, Key> g_ring; // vault_id normalizado -> chave

QString normalizedId(const QString& id)
{
    const QUuid u = QUuid::fromString(id);
    return u.isNull() ? id.toLower() : u.toString(QUuid::WithoutBraces).toLower();
}

std::string wrapAad(const QString& vaultId)
{
    return "synkro-vault-key-v1|" + normalizedId(vaultId).toStdString();
}

std::string normalizedRecovery(const RecoveryBytes& bytes)
{
    const std::string f = formatRecoveryKey(bytes);
    std::string out;
    for (char c : f)
        if (c != '-')
            out += c;
    return out;
}

struct KeyFile {
    std::string keyIdHex;
    std::string salt, nonce, wrapped, tag; // bytes
    std::uint64_t iterations = 0;
};

std::string hex(const std::uint8_t* p, std::size_t n)
{
    static constexpr char h[] = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        s += h[p[i] >> 4];
        s += h[p[i] & 15];
    }
    return s;
}

Result<KeyFile> readKeyFile(const std::wstring& location)
{
    auto raw = win::readWholeFile(keyFilePath(location), 64 * 1024);
    if (!raw)
        return raw.error();
    const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(raw.value()));
    const QJsonObject o = doc.object();
    if (o.value("format").toString() != QLatin1String(kKeyFormat))
        return Error::make(ErrorCode::Corrupted, "key.json do cofre inválido");
    if (o.value("version").toInt() != 1)
        return Error::make(ErrorCode::Unsupported, "key.json de uma versão mais nova do Synkro");
    KeyFile k;
    k.keyIdHex = o.value("key_id").toString().toStdString();
    const QJsonObject kdf = o.value("kdf").toObject();
    k.iterations = static_cast<std::uint64_t>(kdf.value("iterations").toDouble());
    auto salt = fromBase64(kdf.value("salt").toString().toStdString());
    const QJsonObject w = o.value("wrapped_key").toObject();
    auto nonce = fromBase64(w.value("nonce").toString().toStdString());
    auto ct = fromBase64(w.value("ciphertext").toString().toStdString());
    auto tag = fromBase64(w.value("tag").toString().toStdString());
    if (!salt || !nonce || !ct || !tag || nonce.value().size() != 12 || ct.value().size() != 32 || tag.value().size() != 16
        || k.iterations < 1000)
        return Error::make(ErrorCode::Corrupted, "key.json do cofre incompleto");
    k.salt = salt.value();
    k.nonce = nonce.value();
    k.wrapped = ct.value();
    k.tag = tag.value();
    return k;
}

Result<Key> keyFromMachine(const QString& vaultId, const std::wstring& secretsFile)
{
    auto v = SecretStore(secretsFile.empty() ? synkro::secretsFile() : secretsFile).get(secretNameFor(vaultId));
    if (!v)
        return v.error();
    auto raw = fromBase64(v.value());
    synkro::wipe(v.value());
    if (!raw || raw.value().size() != 32)
        return Error::make(ErrorCode::Corrupted, "chave do cofre guardada nesta máquina está inválida");
    Key k{};
    std::memcpy(k.data(), raw.value().data(), 32);
    synkro::wipe(raw.value());
    return k;
}

} // namespace

std::string formatRecoveryKey(const RecoveryBytes& bytes)
{
    std::string out;
    std::uint32_t buffer = 0;
    int bits = 0;
    int written = 0;
    for (std::uint8_t b : bytes) {
        buffer = (buffer << 8) | b;
        bits += 8;
        while (bits >= 5) {
            if (written > 0 && written % 4 == 0)
                out += '-';
            out += kAlphabet[(buffer >> (bits - 5)) & 31];
            bits -= 5;
            ++written;
        }
    }
    return out; // 160 bits = 32 caracteres, sem sobra
}

Result<RecoveryBytes> parseRecoveryKey(std::string_view text)
{
    std::string s;
    for (char c : text) {
        if (c == '-' || c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (c == '0')
            c = 'O';
        else if (c == '1')
            c = 'I';
        else if (c == '8')
            c = 'B';
        s += c;
    }
    if (s.size() != 32)
        return Error::make(ErrorCode::InvalidArgument, "a chave de recuperação tem 32 letras e números (8 grupos de 4)");
    RecoveryBytes out{};
    std::uint32_t buffer = 0;
    int bits = 0;
    std::size_t pos = 0;
    for (char c : s) {
        const char* p = std::strchr(kAlphabet, c);
        if (!p || c == '\0')
            return Error::make(ErrorCode::InvalidArgument, std::string("caractere inválido na chave de recuperação: ") + c);
        buffer = (buffer << 5) | static_cast<std::uint32_t>(p - kAlphabet);
        bits += 5;
        if (bits >= 8) {
            out[pos++] = static_cast<std::uint8_t>((buffer >> (bits - 8)) & 0xFF);
            bits -= 8;
        }
    }
    return out;
}

std::wstring keyFilePath(const std::wstring& vaultLocation)
{
    return win::joinPath(vault::vaultRoot(vaultLocation), L"key.json");
}

bool isEncrypted(const std::wstring& vaultLocation)
{
    return ops::pathExists(keyFilePath(vaultLocation));
}

std::string secretNameFor(const QString& vaultId)
{
    return "vault-key." + normalizedId(vaultId).toStdString();
}

Result<EnableResult> enableEncryption(const std::wstring& location, const std::wstring& secretsFile)
{
    auto identity = vault::readIdentity(location);
    if (!identity)
        return identity.error();
    if (isEncrypted(location))
        return Error::make(ErrorCode::AlreadyExists, "o cofre já é cifrado");
    if (!ops::listSubdirectories(win::joinPath(vault::vaultRoot(location), L"jobs")).empty())
        return Error::make(ErrorCode::InvalidArgument,
                           "o cofre já tem backups; para cifrar, prepare um cofre novo (os dados existentes ficariam sem cifra)");

    Key dek{};
    RecoveryBytes recovery{};
    std::uint8_t salt[16], nonce[12];
    if (auto st = randomBytes(dek.data(), dek.size()); !st)
        return st.error();
    if (auto st = randomBytes(recovery.data(), recovery.size()); !st)
        return st.error();
    if (auto st = randomBytes(salt, sizeof(salt)); !st)
        return st.error();
    if (auto st = randomBytes(nonce, sizeof(nonce)); !st)
        return st.error();

    std::string password = normalizedRecovery(recovery);
    auto kek = pbkdf2Sha256(password, std::string_view(reinterpret_cast<const char*>(salt), sizeof(salt)), kIterations);
    ::SecureZeroMemory(password.data(), password.size());
    if (!kek)
        return kek.error();
    auto aes = AesGcm::create(kek.value());
    wipe(kek.value());
    if (!aes)
        return aes.error();
    Nonce n{};
    std::memcpy(n.data(), nonce, 12);
    std::array<std::uint8_t, 32> wrapped{};
    Tag tag{};
    if (auto st = aes.value().encrypt(n, wrapAad(identity.value().vaultId), dek.data(), dek.size(), wrapped.data(), tag); !st)
        return st.error();

    auto cipher = FileCipher::fromKey(dek);
    if (!cipher)
        return cipher.error();
    QJsonObject o{{"format", kKeyFormat},
                  {"version", 1},
                  {"algorithm", "AES-256-GCM"},
                  {"key_id", QString::fromStdString(hex(cipher.value()->keyId().data(), 8))},
                  {"kdf", QJsonObject{{"name", "PBKDF2-SHA256"},
                                      {"iterations", static_cast<double>(kIterations)},
                                      {"salt", QString::fromStdString(toBase64(salt, sizeof(salt)))}}},
                  {"wrapped_key", QJsonObject{{"nonce", QString::fromStdString(toBase64(nonce, sizeof(nonce)))},
                                              {"ciphertext", QString::fromStdString(toBase64(wrapped.data(), wrapped.size()))},
                                              {"tag", QString::fromStdString(toBase64(tag.data(), tag.size()))}}},
                  {"created_at", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    if (auto st = win::writeFileAtomically(keyFilePath(location), QJsonDocument(o).toJson().toStdString()); !st)
        return st.error();

    EnableResult r;
    r.recoveryKey = formatRecoveryKey(recovery);
    ::SecureZeroMemory(recovery.data(), recovery.size());
    rememberKey(identity.value().vaultId, dek);
    if (auto st = storeKeyOnMachine(identity.value().vaultId, dek, secretsFile); st)
        r.storedOnMachine = true;
    else
        r.storeError = st.error().message;
    wipe(dek);
    return r;
}

Result<Key> unlockWithRecoveryKey(const std::wstring& location, std::string_view recoveryKey)
{
    auto identity = vault::readIdentity(location);
    if (!identity)
        return identity.error();
    auto file = readKeyFile(location);
    if (!file)
        return file.error();
    auto bytes = parseRecoveryKey(recoveryKey);
    if (!bytes)
        return bytes.error();
    std::string password = normalizedRecovery(bytes.value());
    auto kek = pbkdf2Sha256(password, file.value().salt, file.value().iterations);
    ::SecureZeroMemory(password.data(), password.size());
    if (!kek)
        return kek.error();
    auto aes = AesGcm::create(kek.value());
    wipe(kek.value());
    if (!aes)
        return aes.error();
    Nonce n{};
    std::memcpy(n.data(), file.value().nonce.data(), 12);
    Tag tag{};
    std::memcpy(tag.data(), file.value().tag.data(), 16);
    Key dek{};
    if (auto st = aes.value().decrypt(n, wrapAad(identity.value().vaultId),
                                      reinterpret_cast<const std::uint8_t*>(file.value().wrapped.data()), 32, dek.data(), tag);
        !st)
        return Error::make(ErrorCode::AccessDenied, "chave de recuperação incorreta para este cofre");
    return dek;
}

void rememberKey(const QString& vaultId, const Key& key)
{
    std::lock_guard<std::mutex> g(g_ringMutex);
    g_ring[normalizedId(vaultId)] = key;
}

void forgetRememberedKeys()
{
    std::lock_guard<std::mutex> g(g_ringMutex);
    for (auto& [id, key] : g_ring)
        wipe(key);
    g_ring.clear();
}

Status storeKeyOnMachine(const QString& vaultId, const Key& key, const std::wstring& secretsFile)
{
    std::string b64 = toBase64(key.data(), key.size());
    auto st = SecretStore(secretsFile.empty() ? synkro::secretsFile() : secretsFile).set(secretNameFor(vaultId), b64);
    ::SecureZeroMemory(b64.data(), b64.size());
    return st;
}

Result<std::shared_ptr<const FileCipher>> cipherFor(const std::wstring& location, const std::wstring& secretsFile)
{
    auto identity = vault::readIdentity(location);
    if (!identity)
        return identity.error();
    const QString id = identity.value().vaultId;
    const SecretStore store(secretsFile.empty() ? synkro::secretsFile() : secretsFile);

    if (!isEncrypted(location)) {
        if (store.contains(secretNameFor(id)))
            return Error::make(ErrorCode::Corrupted, "este cofre era cifrado, mas o key.json sumiu; nada será gravado em claro "
                                                     "(restaure o SynkroVault\\key.json ou prepare um cofre novo)");
        return std::shared_ptr<const FileCipher>();
    }

    auto file = readKeyFile(location);
    if (!file)
        return file.error();

    Key key{};
    bool found = false;
    {
        std::lock_guard<std::mutex> g(g_ringMutex);
        auto it = g_ring.find(normalizedId(id));
        if (it != g_ring.end()) {
            key = it->second;
            found = true;
        }
    }
    if (!found) {
        auto k = keyFromMachine(id, secretsFile);
        if (!k)
            return Error::make(ErrorCode::KeyRequired,
                               "o cofre \"" + identity.value().label.toStdString()
                                   + "\" é cifrado e a chave não está disponível nesta máquina; informe a chave de "
                                     "recuperação (synkro-worker vault unlock " + win::toUtf8(location) + ")");
        key = k.value();
    }
    auto cipher = FileCipher::fromKey(key);
    wipe(key);
    if (!cipher)
        return cipher.error();
    if (hex(cipher.value()->keyId().data(), 8) != file.value().keyIdHex)
        return Error::make(ErrorCode::KeyRequired, "a chave guardada nesta máquina não é a deste cofre");
    return cipher;
}

} // namespace synkro::crypto
