#include "AppContext.h"

#include "config/ConfigLoader.h"
#include "core/Paths.h"
#include "platform/windows/Win32.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <shellapi.h>

AppContext& AppContext::instance()
{
    static AppContext ctx;
    return ctx;
}

AppContext::AppContext() : m_configDir(synkro::config::defaultConfigDirectory())
{
    auto st = synkro::StateStore::open(synkro::stateDatabaseFile());
    if (st)
        m_state.emplace(std::move(st).value());
    else
        m_stateError = QString::fromStdString(st.error().message);
}

void AppContext::reload()
{
    synkro::config::LoadOptions opt;
    m_config = synkro::config::loadConfig(m_configDir, opt);
    m_vaults.clear();
    m_alerts.clear();
    if (m_state)
        m_alerts = synkro::refreshAlerts(m_config, *m_state, &m_vaults);
    emit reloaded();
}

int AppContext::severityLevel() const
{
    int level = 0;
    for (const auto& a : m_alerts)
        level = std::max(level, a.severity == synkro::AlertSeverity::Critical ? 2 : 1);
    return level;
}

QString AppContext::workerPath() const
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("synkro-worker.exe"));
}

synkro::Status AppContext::writeConfigFile(const std::wstring& relative, const std::string& text)
{
    const std::wstring path = synkro::win::joinPath(m_configDir, relative);
    if (auto st = synkro::win::createDirectories(synkro::win::parentPath(path)); !st)
        return st;
    auto st = synkro::win::writeFileAtomically(path, text);
    if (!st && st.error().code == synkro::ErrorCode::AccessDenied)
        return synkro::Error::make(synkro::ErrorCode::AccessDenied,
                                   "sem permissão para alterar a configuração; abra o Synkro como administrador");
    return st;
}

std::string AppContext::readConfigFile(const std::wstring& relative) const
{
    auto data = synkro::win::readWholeFile(synkro::win::joinPath(m_configDir, relative), 4u << 20);
    return data ? data.value() : std::string();
}

synkro::Result<unsigned long> AppContext::runWorkerElevated(const QStringList& args)
{
    const std::wstring exe = QDir::toNativeSeparators(workerPath()).toStdWString();
    QStringList quoted;
    for (const auto& a : args)
        quoted << (a.contains(' ') ? QLatin1Char('"') + a + QLatin1Char('"') : a);
    const std::wstring params = quoted.join(' ').toStdWString();

    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = params.c_str();
    info.nShow = SW_HIDE;
    if (!::ShellExecuteExW(&info) || !info.hProcess)
        return synkro::win::lastError("executar o worker como administrador");
    ::WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 0;
    ::GetExitCodeProcess(info.hProcess, &code);
    ::CloseHandle(info.hProcess);
    return static_cast<unsigned long>(code);
}
