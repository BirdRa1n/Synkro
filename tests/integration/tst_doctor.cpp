// Checagem pré-piloto: acusa o que impediria o backup de rodar sozinho.

#include "support/Doctor.h"
#include "vault/Vault.h"

#include "TestFs.h"

#include <QTemporaryDir>
#include <QTest>

using namespace synkro;
using namespace synkro::test;

namespace {

bool has(const std::vector<support::DoctorCheck>& checks, support::CheckLevel level, const std::string& text)
{
    for (const auto& c : checks)
        if (c.level == level && c.title.find(text) != std::string::npos)
            return true;
    return false;
}

support::DoctorOptions options(const QTemporaryDir& dir)
{
    support::DoctorOptions o;
    o.workerExe = L"C:\\Synkro\\synkro-worker.exe";
    o.secretsFile = nativePath(dir.path() + "/secrets.dat");
    o.checkScheduler = false; // não depende do Agendador desta máquina
    return o;
}

} // namespace

class TestDoctor : public QObject {
    Q_OBJECT

private slots:
    void init() { qputenv("SYNKRO_DATA_DIR", m_data.path().toUtf8()); }

    void emptyConfigurationIsNotReady()
    {
        config::ConfigSet set;
        QTemporaryDir dir;
        const auto checks = support::runDoctor(set, options(dir));
        QVERIFY(has(checks, support::CheckLevel::Fail, "Nenhum cofre"));
        QVERIFY(has(checks, support::CheckLevel::Fail, "Nenhuma tarefa"));
        QVERIFY(has(checks, support::CheckLevel::Warn, "Nenhum canal de notificação"));
        QCOMPARE(support::worst(checks), support::CheckLevel::Fail);
    }

    void findsTheUsualMistakes()
    {
        QTemporaryDir dir;
        const std::wstring vaultDir = nativePath(dir.path() + "/cofre");
        QVERIFY(win::createDirectories(vaultDir));
        auto id = vault::initialize(vaultDir, QStringLiteral("HD 1"));
        QVERIFY(id);

        config::ConfigSet set;
        config::VaultConfig hd;
        hd.id = "hd1";
        hd.label = "HD 1";
        hd.vaultId = id.value().vaultId.toStdString();
        hd.path = vaultDir;
        config::VaultConfig away = hd;
        away.id = "hd2";
        away.label = "HD 2";
        away.vaultId = "11111111-1111-4111-8111-111111111111"; // não conectado
        set.global.vaults = {hd, away};

        config::JobConfig files;
        files.id = "pdv";
        files.name = "Arquivos";
        files.type = "files";
        files.vaults = {"hd1"};
        files.files.source = nativePath(dir.path() + "/nao-existe");
        files.files.vss = false;
        files.secrets.push_back({"connection.password", "senha-que-falta", 1});
        set.jobs.push_back(files);

        const auto checks = support::runDoctor(set, options(dir));
        QVERIFY(has(checks, support::CheckLevel::Ok, "\"HD 1\" conectado"));
        QVERIFY(has(checks, support::CheckLevel::Fail, "\"HD 2\" inacessível"));
        QVERIFY(has(checks, support::CheckLevel::Fail, "origem não existe"));
        QVERIFY(has(checks, support::CheckLevel::Warn, "único cofre"));
        QVERIFY(has(checks, support::CheckLevel::Warn, "sem agendamento"));
        QVERIFY(has(checks, support::CheckLevel::Fail, "senha-que-falta"));
        QVERIFY(has(checks, support::CheckLevel::Info, "ainda não executada"));
    }

private:
    QTemporaryDir m_data;
};

QTEST_GUILESS_MAIN(TestDoctor)
#include "tst_doctor.moc"
