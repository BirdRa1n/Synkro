#include "config/ConfigLoader.h"
#include "config/ConfigWriter.h"
#include "config/FlowGraph.h"

#include <QTest>

using namespace synkro::config;
using namespace synkro::config::flow;

namespace {

GlobalConfig twoVaults()
{
    GlobalConfig g;
    for (const char* id : {"ssd", "hdd", "rede"}) {
        VaultConfig v;
        v.id = id;
        v.vaultId = std::string("00000000-0000-4000-8000-00000000000") + static_cast<char>('1' + (id[0] % 5));
        v.label = id;
        g.vaults.push_back(v);
    }
    return g;
}

HookCommand cmd(const std::string& exe)
{
    HookCommand c;
    c.run = {exe};
    return c;
}

JobConfig sampleJob()
{
    JobConfig j;
    j.id = "pdv";
    j.name = "PDV";
    j.type = "files";
    j.files.source = L"C:\\PDV";
    j.vaults = {"hdd", "ssd"};
    ScheduleTrigger daily;
    daily.kind = TriggerKind::Daily;
    daily.atMinutes = 22 * 60;
    ScheduleTrigger boot;
    boot.kind = TriggerKind::OnStartup;
    boot.delaySeconds = 600;
    j.schedule = {daily, boot};
    j.hooks.before = {cmd("parar.cmd"), cmd("limpar.cmd")};
    j.hooks.after = {cmd("religar.cmd")};
    j.hooks.onFailure = {cmd("avisar.cmd")};
    return j;
}

int nodeOf(const Graph& g, NodeKind kind, const std::string& key = {})
{
    for (const auto& n : g.nodes) {
        if (n.kind != kind)
            continue;
        if (key.empty() || n.vaultId == key || (!n.command.run.empty() && n.command.run.front() == key))
            return n.id;
    }
    return 0;
}

} // namespace

class TstFlowGraph : public QObject {
    Q_OBJECT

private slots:
    void roundTripKeepsMeaning()
    {
        const JobConfig j = sampleJob();
        const Graph g = fromJob(j, twoVaults());
        // origem + 3 cofres + 2 gatilhos + 4 comandos
        QCOMPARE(g.nodes.size(), std::size_t(10));
        const Conversion back = toJob(g, j);
        QVERIFY(back.warnings.empty());
        QCOMPARE(back.job.vaults, j.vaults);
        QCOMPARE(back.job.schedule.size(), std::size_t(2));
        QCOMPARE(back.job.hooks.before.size(), std::size_t(2));
        QCOMPARE(back.job.hooks.before[0].run.front(), std::string("parar.cmd"));
        QCOMPARE(back.job.hooks.before[1].run.front(), std::string("limpar.cmd"));
        QCOMPARE(back.job.hooks.after.size(), std::size_t(1));
        QCOMPARE(back.job.hooks.onFailure.size(), std::size_t(1));
        QVERIFY(back.job.hooks.onSuccess.empty());
        QCOMPARE(emitJob(back.job), emitJob(j));
    }

    void linkingAVaultAddsIt()
    {
        JobConfig j = sampleJob();
        Graph g = fromJob(j, twoVaults());
        const int src = g.source()->id;
        const int rede = nodeOf(g, NodeKind::Vault, "rede");
        std::string why;
        QVERIFY2(link(g, src, Port::Out, rede, &why), why.c_str());
        const auto back = toJob(g, j);
        QCOMPARE(back.job.vaults.size(), std::size_t(3));

        // Desligar um cofre tira o cofre da tarefa.
        g.removeEdge({src, Port::Out, nodeOf(g, NodeKind::Vault, "hdd")});
        const auto less = toJob(g, j);
        QCOMPARE(less.job.vaults.size(), std::size_t(2));
        QVERIFY(std::find(less.job.vaults.begin(), less.job.vaults.end(), "hdd") == less.job.vaults.end());
    }

    void rulesRejectNonsense()
    {
        Graph g = fromJob(sampleJob(), twoVaults());
        const int src = g.source()->id;
        const int ssd = nodeOf(g, NodeKind::Vault, "ssd");
        const int trigger = nodeOf(g, NodeKind::Trigger);
        const int parar = nodeOf(g, NodeKind::Command, "parar.cmd");
        const int avisar = nodeOf(g, NodeKind::Command, "avisar.cmd");

        QVERIFY(!canConnect(g, ssd, Port::Out, src));          // cofre não tem saída
        QVERIFY(!canConnect(g, src, Port::Out, trigger));      // gatilho não tem entrada
        QVERIFY(!canConnect(g, src, Port::OnSuccess, ssd));    // cofre só na saída principal
        QVERIFY(!canConnect(g, trigger, Port::Out, ssd));      // gatilho não vai direto ao cofre
        QVERIFY(!canConnect(g, src, Port::Out, ssd));          // repetida
        QVERIFY(!canConnect(g, avisar, Port::Out, parar));     // fecharia ciclo origem→avisar→parar→…→origem
        QVERIFY(!canConnect(g, trigger, Port::Out, avisar));   // gatilho só antes da cópia
        QVERIFY(!g.removeNode(src));                           // a origem fica
    }

    void looseBlocksBecomeWarnings()
    {
        JobConfig j = sampleJob();
        Graph g = fromJob(j, twoVaults());
        Node t;
        t.id = g.nextId();
        t.kind = NodeKind::Trigger;
        t.trigger.kind = TriggerKind::OnLogon;
        g.nodes.push_back(t);
        Node c;
        c.id = g.nextId();
        c.kind = NodeKind::Command;
        c.command = cmd("solto.cmd");
        g.nodes.push_back(c);
        const auto back = toJob(g, j);
        QCOMPARE(back.warnings.size(), std::size_t(2));
        QCOMPARE(back.job.schedule.size(), std::size_t(2));
        QCOMPARE(back.job.hooks.before.size(), std::size_t(2));

        // Ligado ao começo da cadeia, o gatilho novo passa a valer.
        std::string why;
        QVERIFY2(link(g, t.id, Port::Out, nodeOf(g, NodeKind::Command, "parar.cmd"), &why), why.c_str());
        QCOMPARE(toJob(g, j).job.schedule.size(), std::size_t(3));
    }

    void emptyFlowIsManualAndValid()
    {
        JobConfig j = sampleJob();
        Graph g = fromJob(j, twoVaults());
        while (const int id = nodeOf(g, NodeKind::Trigger))
            g.removeNode(id);
        const auto back = toJob(g, j);
        QVERIFY(back.job.schedule.empty());
        std::vector<Diagnostic> d;
        const std::string text = emitJob(back.job);
        QVERIFY(text.find("- manual") != std::string::npos);
        (void)parseJobDocument(text, L"jobs\\pdv.yml", twoVaults(), d);
        for (const auto& x : d)
            QVERIFY2(x.severity != Severity::Error, formatDiagnostic(x).c_str());
    }

    void missingVaultStaysVisible()
    {
        JobConfig j = sampleJob();
        j.vaults = {"sumiu"};
        const Graph g = fromJob(j, twoVaults());
        QVERIFY(nodeOf(g, NodeKind::Vault, "sumiu") != 0);
        QCOMPARE(toJob(g, j).job.vaults, std::vector<std::string>{"sumiu"});
    }
};

QTEST_GUILESS_MAIN(TstFlowGraph)
#include "tst_flowgraph.moc"
