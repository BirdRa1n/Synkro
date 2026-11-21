#include "Cli.h"

#include "config/ConfigLoader.h"
#include "core/Version.h"
#include "platform/windows/Volume.h"
#include "vault/Vault.h"

#include <QDir>
#include <QLocale>

namespace cli {

QTextStream& out()
{
    static QTextStream s(stdout);
    return s;
}

QTextStream& err()
{
    static QTextStream s(stderr);
    return s;
}

std::wstring nativeArg(const QString& path)
{
    return QDir::toNativeSeparators(QDir::cleanPath(QDir(path).absolutePath())).toStdWString();
}

QString humanBytes(std::uint64_t bytes)
{
    return QLocale::system().formattedDataSize(static_cast<qint64>(bytes), 1, QLocale::DataSizeTraditionalFormat);
}

int fail(const synkro::Error& e)
{
    err() << "erro [" << synkro::nameOf(e.code) << "]: " << q(e.message) << Qt::endl;
    return ExitFailure;
}

void usage()
{
    out() << "synkro-worker " << synkro::versionString() << "\n\n"
          << "Uso: synkro-worker <comando> [opções]\n\n"
          << "Cofres\n"
          << "  vault check <disco> [--source P]... [--vault V]...  verifica se o disco pode ser um cofre\n"
          << "  vault init <disco> --label <nome> [--source P]...   verifica e inicializa o cofre\n"
          << "       <disco> pode ser \\\\servidor\\pasta (cofre em rede; --user U --password-secret NOME)\n"
          << "  vault show <disco>                                   mostra a identidade do cofre\n"
          << "  vault init ... --encrypt                             cofre cifrado (mostra a chave de recuperação)\n"
          << "  vault unlock <disco>                                 guarda nesta máquina a chave de um cofre cifrado\n"
          << "\nConfiguração\n"
          << "  config validate | plan [--job ID] | apply [--no-tasks]   [--config-dir D]\n"
          << "  config export --to PACOTE.yml [--jobs a,b]   pacote sem senhas e sem a identidade dos discos\n"
          << "  config import --from PACOTE.yml [--bind ID=DISCO]... [--replace] [--dry-run]\n"
          << "  secret set <nome> [--stdin] | list | delete <nome>\n"
          << "  schedule show --job ID                               XML da tarefa no Agendador\n"
          << "\nExecução\n"
          << "  backup --job ID [--trigger T] [--confirm-mass-change] [--no-shutdown]\n"
          << "  backup --source P --vault V... --job-id ID           backup avulso de arquivos\n"
          << "  maintenance [--scrub]                                aplica mudanças, confere cofres e alertas\n"
          << "  status                                               situação das tarefas, cofres e alertas\n"
          << "  shutdown cancel                                      cancela um desligamento agendado\n"
          << "  autostart on | off | status                          Synkro na bandeja ao entrar no Windows\n"
          << "  notify [send] | test [--channel ID] | list [--limit N]   notificações por e-mail, webhook e Telegram\n"
          << "\nRecuperação\n"
          << "  restore --vault V --job ID --to PASTA [--run R] [--path P]\n"
          << "  restore-db --vault V --job ID --database DB (--target-database NOVO | --to ARQUIVO) [--run R]\n"
          << "  restore-test --job ID [--vault V] [--database DB]   teste de restauração num banco temporário\n"
          << "  runs --vault V --job ID      dumps --vault V --job ID      verify --vault V --job ID [--sample N]\n"
          << "\nDiagnóstico\n"
          << "  doctor                                               este PDV está pronto? (rode como administrador)\n"
          << "  diag [--to ARQUIVO.txt]                              relatório para o suporte (sem senhas)\n"
          << "  hash <arquivo> [--uncached]   scan <pasta> [--ids]   copy <origem> <destino> [--uncached]\n"
          << "\nSaída: 0 sucesso · 1 degradado · 2 falha · 3 bloqueado · 4 já em execução · 64 uso incorreto\n"
          << Qt::endl;
}

bool parseSub(QCommandLineParser& p, const QStringList& args)
{
    p.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
    if (!p.parse(QStringList{QStringLiteral("synkro-worker")} + args)) {
        err() << p.errorText() << Qt::endl;
        return false;
    }
    return true;
}

std::wstring configDirFrom(const QCommandLineParser& p)
{
    const QString dir = p.isSet(QStringLiteral("config-dir")) ? p.value(QStringLiteral("config-dir")) : QString();
    return dir.isEmpty() ? synkro::config::defaultConfigDirectory() : nativeArg(dir);
}

bool loadAndReport(const std::wstring& dir, synkro::config::ConfigSet& set, bool quietWhenClean)
{
    set = synkro::config::loadConfig(dir);
    for (const auto& d : set.diagnostics)
        out() << q(synkro::config::formatDiagnostic(d)) << "\n";
    if (!set.diagnostics.empty() || !quietWhenClean) {
        out() << "\n" << set.errorCount() << " erro(s), " << set.warningCount() << " aviso(s) em " << q(dir) << ".";
        if (set.hasErrors())
            out() << " Configuração NÃO aplicada.";
        out() << Qt::endl;
    }
    return !set.hasErrors();
}

void printBackupResult(const synkro::BackupResult& r)
{
    out() << "\nexecução " << q(r.runId) << ": " << synkro::nameOf(r.status) << "\n";
    if (!r.error.empty())
        out() << "  " << q(r.error) << "\n";
    if (!r.snapshotNote.empty())
        out() << "  snapshot VSS não usado: " << q(r.snapshotNote) << "\n";
    else if (r.snapshotUsed)
        out() << "  lido de um snapshot VSS\n";
    out() << "  origem: " << r.filesScanned << " item(ns), " << humanBytes(r.bytesScanned) << "\n";
    for (const auto& w : r.warnings)
        out() << "  aviso: " << q(w) << "\n";
    for (const auto& h : r.hooks) {
        if (!h.ok)
            out() << "  hook " << q(h.phase) << (h.critical ? " FALHOU: " : " falhou (aviso): ") << q(h.command) << " — "
                  << q(h.message) << "\n";
    }
    for (const auto& v : r.vaults) {
        out() << "  cofre " << (v.vaultId.isEmpty() ? q(v.location) : v.vaultId) << " [" << synkro::nameOf(v.status) << "]";
        if (!v.location.empty())
            out() << " em " << q(v.location);
        out() << "\n";
        if (!v.error.empty())
            out() << "    erro: " << q(v.error) << "\n";
        if (v.guard.blocked())
            out() << "    BLOQUEADO (" << synkro::nameOf(v.guard.code) << "): " << q(v.guard.message)
                  << "\n    Revise a origem; para aplicar mesmo assim use --confirm-mass-change.\n";
        if (v.status == synkro::VaultRunStatus::Success || v.status == synkro::VaultRunStatus::Partial)
            out() << "    novos " << v.added << " · alterados " << v.modified << " · excluídos " << v.deleted
                  << " · inalterados " << v.unchanged << " · falhas " << v.failed << " · gravados "
                  << humanBytes(v.bytesWritten) << (v.recoveredRuns ? " · execução interrompida recuperada" : "") << "\n";
        for (const auto& [path, msg] : v.fileErrors)
            out() << "    ! " << q(path) << ": " << q(msg) << "\n";
    }
    out().flush();
}

int exitFor(synkro::RunStatus s)
{
    switch (s) {
    case synkro::RunStatus::Success:        return ExitOk;
    case synkro::RunStatus::Degraded:       return ExitDegraded;
    case synkro::RunStatus::Blocked:        return ExitBlocked;
    case synkro::RunStatus::AlreadyRunning: return ExitAlreadyRunning;
    case synkro::RunStatus::Failed:         break;
    }
    return ExitFailure;
}

std::wstring vaultLocationArg(const QString& v)
{
    const std::wstring path = nativeArg(v);
    if (synkro::vault::readIdentity(path))
        return path;
    auto vol = synkro::win::queryVolume(path);
    return vol ? vol.value().mountPoint : path;
}

} // namespace cli
