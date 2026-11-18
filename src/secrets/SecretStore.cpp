#include "secrets/SecretStore.h"

#include "platform/windows/Snapshot.h"
#include "platform/windows/Win32.h"

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <wincrypt.h>
#include <sddl.h>

namespace synkro {

namespace {

constexpr char kFormat[] = "synkro-secrets";
// Entropia adicional: um valor copiado para outro programa não é decifrado
// sem conhecê-la.
constexpr char kEntropy[] = "Synkro.SecretStore.v1";

Result<QJsonObject> loadDocument(const std::wstring& path)
{
    auto data = win::readWholeFile(path, 4u << 20);
    if (!data) {
        if (data.error().code == ErrorCode::NotFound)
            return QJsonObject{};
        return data.error();
    }
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(data.value()), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()
        || doc.object().value(QStringLiteral("format")).toString() != QLatin1String(kFormat))
        return Error::make(ErrorCode::Corrupted, "arquivo de segredos inválido");
    return doc.object();
}

void restrictToAdministrators(const std::wstring& path)
{
    // Somente um processo elevado pode (e deve) fechar a ACL; caso contrário
    // o próprio usuário perderia o acesso ao arquivo que acabou de criar.
    if (!win::isProcessElevated())
        return;
    PSECURITY_DESCRIPTOR sd = nullptr;
    // P = protegida (sem herança); SY = SYSTEM; BA = Administradores.
    if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1,
                                                               &sd, nullptr)) {
        ::SetFileSecurityW(win::toExtendedPath(path).c_str(), DACL_SECURITY_INFORMATION, sd);
        ::LocalFree(sd);
    }
}

Status saveDocument(const std::wstring& path, QJsonObject doc)
{
    doc.insert(QStringLiteral("format"), QString::fromLatin1(kFormat));
    doc.insert(QStringLiteral("format_version"), 1);
    const QByteArray data = QJsonDocument(doc).toJson(QJsonDocument::Indented);
    const std::wstring parent = win::parentPath(path);
    if (!parent.empty()) {
        if (auto st = win::createDirectories(parent); !st)
            return st;
    }
    if (auto st = win::writeFileAtomically(path, std::string_view(data.constData(), static_cast<size_t>(data.size()))); !st)
        return st;
    restrictToAdministrators(path);
    return Status::success();
}

} // namespace

void wipe(std::string& s)
{
    if (!s.empty())
        ::SecureZeroMemory(s.data(), s.size());
    s.clear();
}

SecretStore::SecretStore(std::wstring path) : m_path(std::move(path)) {}

bool SecretStore::isValidName(const std::string& name)
{
    if (name.empty() || name.size() > 64)
        return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok)
            return false;
    }
    return true;
}

Status SecretStore::set(const std::string& name, const std::string& value)
{
    if (!isValidName(name))
        return Error::make(ErrorCode::InvalidArgument, "nome de segredo inválido (use a-z, 0-9, '-', '_' e '.')");
    if (value.empty())
        return Error::make(ErrorCode::InvalidArgument, "o valor do segredo está vazio");

    DATA_BLOB in{static_cast<DWORD>(value.size()), reinterpret_cast<BYTE*>(const_cast<char*>(value.data()))};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropy) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    DATA_BLOB out{};
    if (!::CryptProtectData(&in, L"Synkro", &entropy, nullptr, nullptr,
                            CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN, &out))
        return win::lastError("cifrar segredo (DPAPI)");
    const QByteArray cipher(reinterpret_cast<const char*>(out.pbData), static_cast<qsizetype>(out.cbData));
    ::LocalFree(out.pbData);

    auto doc = loadDocument(m_path);
    if (!doc)
        return doc.error();
    QJsonObject secrets = doc.value().value(QStringLiteral("secrets")).toObject();
    secrets.insert(QString::fromStdString(name), QString::fromLatin1(cipher.toBase64()));
    QJsonObject updated = doc.value();
    updated.insert(QStringLiteral("secrets"), secrets);
    return saveDocument(m_path, updated);
}

Result<std::string> SecretStore::get(const std::string& name) const
{
    auto doc = loadDocument(m_path);
    if (!doc)
        return doc.error();
    const QJsonValue v = doc.value().value(QStringLiteral("secrets")).toObject().value(QString::fromStdString(name));
    if (!v.isString())
        return Error::make(ErrorCode::NotFound, "segredo \"" + name + "\" não cadastrado (synkro-worker secret set " + name + ")");

    QByteArray cipher = QByteArray::fromBase64(v.toString().toLatin1());
    DATA_BLOB in{static_cast<DWORD>(cipher.size()), reinterpret_cast<BYTE*>(cipher.data())};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropy) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    DATA_BLOB out{};
    if (!::CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        return win::lastError("decifrar segredo \"" + name + "\" (DPAPI)");
    std::string value(reinterpret_cast<const char*>(out.pbData), out.cbData);
    ::SecureZeroMemory(out.pbData, out.cbData);
    ::LocalFree(out.pbData);
    return value;
}

Status SecretStore::remove(const std::string& name)
{
    auto doc = loadDocument(m_path);
    if (!doc)
        return doc.error();
    QJsonObject secrets = doc.value().value(QStringLiteral("secrets")).toObject();
    if (!secrets.contains(QString::fromStdString(name)))
        return Error::make(ErrorCode::NotFound, "segredo \"" + name + "\" não cadastrado");
    secrets.remove(QString::fromStdString(name));
    QJsonObject updated = doc.value();
    updated.insert(QStringLiteral("secrets"), secrets);
    return saveDocument(m_path, updated);
}

Result<std::vector<std::string>> SecretStore::names() const
{
    auto doc = loadDocument(m_path);
    if (!doc)
        return doc.error();
    std::vector<std::string> out;
    for (const QString& k : doc.value().value(QStringLiteral("secrets")).toObject().keys())
        out.push_back(k.toStdString());
    return out;
}

bool SecretStore::contains(const std::string& name) const
{
    auto n = names();
    if (!n)
        return false;
    for (const auto& x : n.value()) {
        if (x == name)
            return true;
    }
    return false;
}

} // namespace synkro
