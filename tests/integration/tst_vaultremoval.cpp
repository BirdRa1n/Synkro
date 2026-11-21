// Remover um cofre da configuração: o que é regravado e quando é recusado.

#include "config/ConfigLoader.h"
#include "config/ConfigWriter.h"
#include "config/VaultRemoval.h"

#include <QTest>

#include <map>

using namespace synkro;
using namespace synkro::config;

namespace {

VaultConfig vault(const std::string& id)
{
    VaultConfig v;
    v.id = id;
    v.label = "Disco " + id;
    v.vaultId = id == "a" ? "11111111-1111-4111-8111-111111111111" : id == "b" ? "22222222-2222-4222-8222-222222222222"
                                                                              : "33333333-3333-4333-8333-333333333333";
    return v;
}

JobConfig job(const std::string& id, std::vector<std::string> vaults, int minOk = 0)
{
    JobConfig j;
    j.id = id;
    j.name = "Tarefa " + id;
    j.type = "files";
    j.file = L"jobs\\" + std::wstring(id.begin(), id.end()) + L".yml";
    j.vaults = std::move(vaults);
    j.minVaultsOk = minOk;
    j.files.source = L"C:\\PDV";
    return j;
}

// Configuração "gravada pelo desktop" (arquivos íntegros).
struct Files {
    ConfigSet set;
    std::map<std::wstring, std::string> text;

    std::function<std::string(const std::wstring&)> reader() const
    {
        return [this](const std::wstring& rel) {
            const auto it = text.find(rel);
            return it == text.end() ? std::string() : it->second;
        };
    }
};

Files sample()
{
    Files f;
    f.set.global.vaults = {vault("a"), vault("b"), vault("c")};
    f.set.global.defaultVaults = {"a", "b"};
    f.set.jobs = {job("pdv", {"a", "b"}, 2), job("banco", {"b", "c"}), job("so-a", {"a"})};
    f.text[L"synkro.yml"] = emitGlobal(f.set.global);
    for (const auto& j : f.set.jobs)
        f.text[j.file] = emitJob(j);
    return f;
}

} // namespace

class TestVaultRemoval : public QObject {
    Q_OBJECT

private slots:
    void removesFromJobsAndGlobal()
    {
        Files f = sample();
        auto plan = planVaultRemoval(f.set, "b", f.reader());
        QVERIFY(plan);
        QVERIFY(plan.value().possible());
        QCOMPARE(plan.value().affectedJobs, (std::vector<std::string>{"Tarefa pdv", "Tarefa banco"}));
        QCOMPARE(plan.value().files.size(), std::size_t(3));
        QCOMPARE(plan.value().files.back().first, std::wstring(L"synkro.yml")); // por último

        std::vector<Diagnostic> d;
        const GlobalConfig g = parseGlobalDocument(plan.value().files.back().second, L"synkro.yml", d);
        QCOMPARE(g.vaults.size(), std::size_t(2));
        QVERIFY(!g.findVault("b"));
        QCOMPARE(g.defaultVaults, std::vector<std::string>{"a"});

        const JobConfig pdv = parseJobDocument(plan.value().files[0].second, L"jobs\\pdv.yml", g, d);
        QCOMPARE(pdv.vaults, std::vector<std::string>{"a"});
        QCOMPARE(pdv.minVaultsOk, 0); // pedia 2, sobrou 1: passa a "todos"
    }

    void refusesWhenAJobWouldHaveNoVault()
    {
        Files f = sample();
        auto plan = planVaultRemoval(f.set, "a", f.reader());
        QVERIFY(plan);
        QVERIFY(!plan.value().possible());
        QVERIFY(plan.value().files.empty()); // tudo ou nada
        QVERIFY(plan.value().blockers.front().find("Tarefa so-a") != std::string::npos);
    }

    void refusesHandEditedFiles()
    {
        Files f = sample();
        f.text[L"jobs\\banco.yml"] += "# comentário do técnico\n";
        auto plan = planVaultRemoval(f.set, "c", f.reader());
        QVERIFY(plan);
        QVERIFY(!plan.value().possible());
        QVERIFY(plan.value().blockers.front().find("editado à mão") != std::string::npos);

        QVERIFY(!planVaultRemoval(f.set, "nao-existe", f.reader()));
    }
};

QTEST_GUILESS_MAIN(TestVaultRemoval)
#include "tst_vaultremoval.moc"
