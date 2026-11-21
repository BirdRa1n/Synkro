#include "config/ConfigLoader.h"
#include "config/ConfigWriter.h"

#include <QTest>

using namespace synkro::config;

namespace {

QString dump(const std::vector<Diagnostic>& d)
{
    QString s;
    for (const auto& x : d)
        s += QString::fromStdString(formatDiagnostic(x)) + "\n";
    return s;
}

GlobalConfig sampleGlobal()
{
    GlobalConfig g;
    g.instanceName = "PDV \"Matriz\"";
    VaultConfig a;
    a.id = "hd-externo-1";
    a.vaultId = "7f1c9a2e-3b0d-4c7e-9a51-0d2b6e1f4a10";
    a.label = "HD Externo 1";
    a.path = L"E:\\";
    a.rotation = true;
    a.maxAbsenceSeconds = 7 * 86400;
    g.vaults.push_back(a);
    VaultConfig b = a;
    b.id = "hd-interno-2";
    b.vaultId = "2b8e5d71-9c44-4f3a-8e0b-6a7c1d9f3e22";
    b.label = "HD Interno 2";
    b.rotation = false;
    b.path.clear();
    g.vaults.push_back(b);
    g.defaultVaults = {"hd-externo-1", "hd-interno-2"};
    ScheduleTrigger scrub;
    scrub.kind = TriggerKind::Weekly;
    scrub.days = {7};
    scrub.atMinutes = 180;
    g.scrubSchedule = scrub;
    g.shutdownAllowedGroups = {"noite"};
    return g;
}

} // namespace

class TstConfigWriter : public QObject {
    Q_OBJECT

private slots:
    void globalRoundTrip()
    {
        const GlobalConfig g = sampleGlobal();
        const std::string text = emitGlobal(g);
        QVERIFY(isPristineGenerated(text));

        std::vector<Diagnostic> d;
        const GlobalConfig back = parseGlobalDocument(text, L"synkro.yml", d);
        QVERIFY2(d.empty(), qPrintable(dump(d) + QString::fromStdString(text)));
        QCOMPARE(back.instanceName, g.instanceName);
        QCOMPARE(back.vaults.size(), size_t(2));
        QCOMPARE(back.vaults[0].path, std::wstring(L"E:\\"));
        QVERIFY(back.vaults[0].rotation);
        QCOMPARE(back.defaultVaults, g.defaultVaults);
        QVERIFY(back.scrubSchedule.has_value());
        QCOMPARE(back.scrubSchedule->days, std::vector<int>{7});
        QCOMPARE(back.shutdownAllowedGroups, g.shutdownAllowedGroups);
    }

    void filesJobRoundTrip()
    {
        JobConfig j;
        j.id = "pdv-arquivos";
        j.name = "Arquivos do PDV";
        j.type = "files";
        j.group = "noite";
        j.vaults = {"hd-externo-1"};
        ScheduleTrigger w;
        w.kind = TriggerKind::Weekly;
        w.days = {1, 2, 3, 4, 5};
        w.atMinutes = 22 * 60 + 30;
        ScheduleTrigger e;
        e.kind = TriggerKind::Every;
        e.intervalSeconds = 7200;
        e.betweenStart = 8 * 60;
        e.betweenEnd = 22 * 60;
        j.schedule = {w, e};
        j.files.source = L"C:\\PDV";
        j.files.exclude = {L"Logs/**", L"*.tmp"};
        j.files.sentinelFile = L"Config/pdv.ini";
        j.hooks.before.push_back({{"C:/PDV/fechar.exe", "--silencioso"}, 300, true});
        j.shutdownAfter = true;

        const std::string text = emitJob(j);
        std::vector<Diagnostic> d;
        const JobConfig back = parseJobDocument(text, L"jobs\\pdv-arquivos.yml", sampleGlobal(), d);
        QVERIFY2(d.empty(), qPrintable(dump(d) + QString::fromStdString(text)));
        QCOMPARE(back.files.source, std::wstring(L"C:\\PDV"));
        QCOMPARE(back.files.exclude.size(), size_t(2));
        QCOMPARE(back.schedule.size(), size_t(2));
        QCOMPARE(back.schedule[1].betweenEnd, 22 * 60);
        QCOMPARE(back.hooks.before.size(), size_t(1));
        QVERIFY(back.shutdownAfter);
        QVERIFY(text.find("\nsource: ") != std::string::npos); // uma pasta: formato de sempre
    }

    void filesJobWithSeveralSourcesRoundTrip()
    {
        JobConfig j;
        j.id = "pdv-arquivos";
        j.name = "Arquivos do PDV";
        j.type = "files";
        j.vaults = {"hd-externo-1"};
        j.files.source = L"C:\\PDV";
        j.files.moreSources.push_back({L"D:\\Fiscal", {}, L"Fiscal"});
        j.files.moreSources.push_back({L"E:\\Exportacao\\Notas", L"notas-exportadas", L"notas-exportadas"});

        const std::string text = emitJob(j);
        QVERIFY(text.find("sources:") != std::string::npos);
        std::vector<Diagnostic> d;
        const JobConfig back = parseJobDocument(text, L"jobs\\pdv-arquivos.yml", sampleGlobal(), d);
        QVERIFY2(d.empty(), qPrintable(dump(d) + QString::fromStdString(text)));
        QCOMPARE(back.files.source, std::wstring(L"C:\\PDV"));
        QCOMPARE(back.files.moreSources.size(), size_t(2));
        QCOMPARE(back.files.moreSources[0].alias, std::wstring(L"Fiscal"));
        QCOMPARE(back.files.moreSources[1].name, std::wstring(L"notas-exportadas"));
        QVERIFY(emitJob(back).find("  - { path: \"E:/Exportacao/Notas\", name: \"notas-exportadas\" }") != std::string::npos);
        QVERIFY(emitJob(back).find("  - \"D:/Fiscal\"\n") != std::string::npos);
    }

    void databaseJobRoundTrip()
    {
        JobConfig j;
        j.id = "pdv-postgres";
        j.name = "Banco PDV";
        j.type = "database";
        j.database.engine = "postgresql";
        j.database.host = "localhost";
        j.database.port = 5432;
        j.database.user = "synkro_backup";
        j.database.password = {SecretRef::Kind::Secret, "pg-pdv"};
        j.database.sslMode = "prefer";
        j.database.databases = {{"pdv_vendas", {"public.log_*"}, {}}};
        ScheduleTrigger daily;
        daily.kind = TriggerKind::Daily;
        daily.atMinutes = 22 * 60;
        j.schedule = {daily};

        const std::string text = emitJob(j);
        QVERIFY(text.find("pg-pdv") != std::string::npos);
        std::vector<Diagnostic> d;
        const JobConfig back = parseJobDocument(text, L"jobs\\pdv-postgres.yml", sampleGlobal(), d);
        QVERIFY2(d.empty(), qPrintable(dump(d) + QString::fromStdString(text)));
        QCOMPARE(back.database.password.name, std::string("pg-pdv"));
        QCOMPARE(back.database.databases.front().excludeTables.front(), std::string("public.log_*"));
    }

    void handEditDetected()
    {
        std::string text = emitGlobal(sampleGlobal());
        QVERIFY(isPristineGenerated(text));
        std::string crlf;
        for (char c : text) {
            if (c == '\n')
                crlf += '\r';
            crlf += c;
        }
        QVERIFY(isPristineGenerated(crlf)); // só a quebra de linha mudou
        text += "# meu comentário\n";
        QVERIFY(!isPristineGenerated(text));
        QVERIFY(!isPristineGenerated("version: 1\n"));
    }

    void durations()
    {
        QCOMPARE(formatDuration(90 * 86400), std::string("90d"));
        QCOMPARE(formatDuration(7200), std::string("2h"));
        QCOMPARE(formatDuration(600), std::string("10m"));
        QCOMPARE(formatDuration(45), std::string("45s"));
    }

    // Inserir um cofre num synkro.yml editado à mão: só a lista vaults muda.
    void insertVaultPreservesHandEditedFile()
    {
        VaultConfig nv;
        nv.id = "hd-novo";
        nv.vaultId = "9d3f6a10-5e2b-4c8d-b7a1-3f0e9c2d4b55";
        nv.label = "HD Novo";
        nv.path = L"F:\\";
        nv.maxAbsenceSeconds = 86400;

        const std::string hand =
            "# Configuração da loja — feita pelo técnico\n"
            "version: 1\n"
            "\n"
            "vaults:\n"
            "    # disco que fica no PDV\n"
            "    - id: hd-externo-1\n"
            "      vault_id: 7f1c9a2e-3b0d-4c7e-9a51-0d2b6e1f4a10\n"
            "      label: \"HD Externo 1\"\n"
            "\n"
            "defaults:\n"
            "  vaults: [hd-externo-1]   # padrão\n";
        auto out = insertVaultIntoGlobalYaml(hand, nv);
        QVERIFY(out);
        const std::string& t = out.value();
        QVERIFY(t.find("# Configuração da loja — feita pelo técnico\n") == 0);
        QVERIFY(t.find("    # disco que fica no PDV\n") != std::string::npos);
        QVERIFY(t.find("  vaults: [hd-externo-1]   # padrão\n") != std::string::npos);
        QVERIFY(t.find("    - id: hd-novo\n") != std::string::npos); // mesma indentação da lista
        QVERIFY(t.find("- id: hd-novo") < t.find("defaults:"));
        std::vector<Diagnostic> d;
        const GlobalConfig g = parseGlobalDocument(t, L"synkro.yml", d);
        QVERIFY2(std::none_of(d.begin(), d.end(), [](const Diagnostic& x) { return x.severity == Severity::Error; }),
                 qPrintable(dump(d)));
        QCOMPARE(g.vaults.size(), std::size_t(2));
        QVERIFY(g.findVault("hd-novo") && g.findVault("hd-novo")->path == L"F:\\");
        QCOMPARE(g.defaultVaults, std::vector<std::string>{"hd-externo-1"}); // padrões intactos
    }

    void insertVaultOtherShapes()
    {
        VaultConfig nv;
        nv.id = "nas";
        nv.vaultId = "9d3f6a10-5e2b-4c8d-b7a1-3f0e9c2d4b55";
        nv.label = "NAS";
        nv.maxAbsenceSeconds = 86400;
        auto parsesWithNas = [](const std::string& t) {
            std::vector<Diagnostic> d;
            const GlobalConfig g = parseGlobalDocument(t, L"synkro.yml", d);
            return g.findVault("nas") != nullptr
                   && std::none_of(d.begin(), d.end(), [](const Diagnostic& x) { return x.severity == Severity::Error; });
        };

        // Lista vazia em linha.
        auto empty = insertVaultIntoGlobalYaml("version: 1\nvaults: []\n", nv);
        QVERIFY(empty && parsesWithNas(empty.value()));
        QVERIFY(empty.value().find("[]") == std::string::npos);

        // Itens na coluna 0 ("- id:" logo abaixo de vaults:).
        auto col0 = insertVaultIntoGlobalYaml(
            "version: 1\nvaults:\n- id: a\n  vault_id: 7f1c9a2e-3b0d-4c7e-9a51-0d2b6e1f4a10\nlogging:\n  level: info\n", nv);
        QVERIFY(col0 && parsesWithNas(col0.value()));
        QVERIFY(col0.value().find("\n- id: nas\n") != std::string::npos);

        // Sem a chave vaults: é criada no fim.
        auto none = insertVaultIntoGlobalYaml("version: 1\nlogging:\n  level: info", nv);
        QVERIFY(none && parsesWithNas(none.value()));

        // Quebras de linha CRLF são mantidas.
        auto crlf = insertVaultIntoGlobalYaml("version: 1\r\nvaults: []\r\n", nv);
        QVERIFY(crlf);
        QVERIFY(crlf.value().find("\r\n  - id: nas\r\n") != std::string::npos);

        // Lista em linha com itens: não edita (seria arriscado).
        QVERIFY(!insertVaultIntoGlobalYaml("version: 1\nvaults: [{id: a}]\n", nv));
    }
};

QTEST_GUILESS_MAIN(TstConfigWriter)
#include "tst_configwriter.moc"
