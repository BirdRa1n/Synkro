#include "vault/Vault.h"

#include "core/Version.h"
#include "platform/windows/Win32.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSysInfo>
#include <QUuid>

namespace synkro::vault {

std::wstring vaultRoot(const std::wstring& location)
{
    return win::joinPath(location, kVaultDirName);
}

QByteArray serialize(const VaultIdentity& identity)
{
    QJsonObject o;
    o.insert(QStringLiteral("format"), QString::fromLatin1(kFormatName));
    o.insert(QStringLiteral("format_version"), identity.formatVersion);
    o.insert(QStringLiteral("vault_id"), identity.vaultId);
    o.insert(QStringLiteral("label"), identity.label);
    o.insert(QStringLiteral("created_at"), identity.createdAt.toUTC().toString(Qt::ISODate));
    o.insert(QStringLiteral("created_by_host"), identity.createdByHost);
    o.insert(QStringLiteral("synkro_version"), identity.synkroVersion);
    return QJsonDocument(o).toJson(QJsonDocument::Indented);
}

Result<VaultIdentity> parse(const QByteArray& json)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return Error::make(ErrorCode::Corrupted, "vault.json inválido: " + err.errorString().toStdString());

    const QJsonObject o = doc.object();
    if (o.value(QStringLiteral("format")).toString() != QLatin1String(kFormatName))
        return Error::make(ErrorCode::Corrupted, "vault.json não é um arquivo de cofre do Synkro");

    VaultIdentity id;
    id.formatVersion = o.value(QStringLiteral("format_version")).toInt(0);
    if (id.formatVersion <= 0)
        return Error::make(ErrorCode::Corrupted, "vault.json sem format_version válido");
    if (id.formatVersion > kVaultFormatVersion)
        return Error::make(ErrorCode::Unsupported,
                           "cofre criado por uma versão mais nova do Synkro (formato "
                               + std::to_string(id.formatVersion) + ")");

    id.vaultId = o.value(QStringLiteral("vault_id")).toString();
    if (QUuid::fromString(id.vaultId).isNull())
        return Error::make(ErrorCode::Corrupted, "vault.json com vault_id inválido");

    id.label = o.value(QStringLiteral("label")).toString();
    id.createdAt = QDateTime::fromString(o.value(QStringLiteral("created_at")).toString(), Qt::ISODate);
    id.createdByHost = o.value(QStringLiteral("created_by_host")).toString();
    id.synkroVersion = o.value(QStringLiteral("synkro_version")).toString();
    return id;
}

Result<VaultIdentity> initialize(const std::wstring& location, const QString& label)
{
    const std::wstring root = vaultRoot(location);
    const std::wstring identityPath = win::joinPath(root, kIdentityFileName);

    if (::GetFileAttributesW(win::toExtendedPath(identityPath).c_str()) != INVALID_FILE_ATTRIBUTES)
        return Error::make(ErrorCode::AlreadyExists, "este disco já contém um cofre do Synkro");

    if (auto st = win::createDirectories(win::joinPath(root, L"jobs")); !st)
        return st.error();

    VaultIdentity id;
    id.vaultId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    id.label = label;
    id.createdAt = QDateTime::currentDateTimeUtc();
    id.createdByHost = QSysInfo::machineHostName();
    id.synkroVersion = QString::fromLatin1(versionString());
    id.formatVersion = kVaultFormatVersion;

    const QByteArray json = serialize(id);
    if (auto st = win::writeFileAtomically(identityPath, std::string_view(json.constData(), static_cast<size_t>(json.size())));
        !st)
        return st.error();
    return id;
}

Result<VaultIdentity> readIdentity(const std::wstring& location)
{
    auto data = win::readWholeFile(win::joinPath(vaultRoot(location), kIdentityFileName), 1u << 20);
    if (!data)
        return data.error();
    return parse(QByteArray::fromStdString(data.value()));
}

} // namespace synkro::vault
