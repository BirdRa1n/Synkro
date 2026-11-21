#include "config/FlowGraph.h"

#include <algorithm>
#include <functional>
#include <set>

namespace synkro::config::flow {

namespace {

constexpr double kColumn = 300.0;
constexpr double kRow = 120.0;

bool isCommandLike(const Node* n)
{
    return n && (n->kind == NodeKind::Command || n->kind == NodeKind::Source);
}

// true se `to` alcança `from` seguindo as ligações (a nova ligação fecharia um ciclo).
bool reaches(const Graph& g, int start, int target)
{
    std::set<int> seen;
    std::function<bool(int)> walk = [&](int id) {
        if (id == target)
            return true;
        if (!seen.insert(id).second)
            return false;
        for (const auto& e : g.edges)
            if (e.from == id && walk(e.to))
                return true;
        return false;
    };
    return walk(start);
}

// Cadeia de comandos que termina na entrada da origem (ordem de execução).
std::vector<int> beforeChain(const Graph& g, int sourceId)
{
    std::vector<int> chain;
    int current = sourceId;
    std::set<int> seen{sourceId};
    for (;;) {
        int prev = 0;
        for (const auto& e : g.edges) {
            const Node* n = g.find(e.from);
            if (e.to == current && n && n->kind == NodeKind::Command) {
                prev = e.from;
                break;
            }
        }
        if (!prev || !seen.insert(prev).second)
            break;
        chain.push_back(prev);
        current = prev;
    }
    std::reverse(chain.begin(), chain.end());
    return chain;
}

// Comandos que saem de uma porta da origem: cada ramo direto (de cima para
// baixo) seguido da sua cadeia.
std::vector<int> afterChain(const Graph& g, int sourceId, Port port)
{
    std::vector<const Node*> heads;
    for (const auto& e : g.edges)
        if (e.from == sourceId && e.port == port)
            if (const Node* n = g.find(e.to); n && n->kind == NodeKind::Command)
                heads.push_back(n);
    std::stable_sort(heads.begin(), heads.end(), [](const Node* a, const Node* b) { return a->y < b->y; });
    std::vector<int> out;
    std::set<int> seen;
    for (const Node* head : heads) {
        int current = head->id;
        while (current && seen.insert(current).second) {
            out.push_back(current);
            int next = 0;
            for (const auto& e : g.edges) {
                const Node* n = g.find(e.to);
                if (e.from == current && n && n->kind == NodeKind::Command) {
                    next = e.to;
                    break;
                }
            }
            current = next;
        }
    }
    return out;
}

} // namespace

const Node* Graph::find(int id) const
{
    for (const auto& n : nodes)
        if (n.id == id)
            return &n;
    return nullptr;
}

Node* Graph::find(int id)
{
    for (auto& n : nodes)
        if (n.id == id)
            return &n;
    return nullptr;
}

const Node* Graph::source() const
{
    for (const auto& n : nodes)
        if (n.kind == NodeKind::Source)
            return &n;
    return nullptr;
}

int Graph::nextId() const
{
    int id = 0;
    for (const auto& n : nodes)
        id = std::max(id, n.id);
    return id + 1;
}

bool Graph::removeNode(int id)
{
    const Node* n = find(id);
    if (!n || n->kind == NodeKind::Source)
        return false;
    edges.erase(std::remove_if(edges.begin(), edges.end(), [id](const Edge& e) { return e.from == id || e.to == id; }),
                edges.end());
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [id](const Node& x) { return x.id == id; }), nodes.end());
    return true;
}

void Graph::removeEdge(const Edge& e)
{
    edges.erase(std::remove(edges.begin(), edges.end(), e), edges.end());
}

Graph fromJob(const JobConfig& job, const GlobalConfig& global)
{
    Graph g;
    int next = 1;

    Node src;
    src.id = next++;
    src.kind = NodeKind::Source;
    g.nodes.push_back(src);
    const int sourceId = src.id;

    // Comandos "antes", em cadeia até a origem.
    std::vector<int> before;
    for (const auto& c : job.hooks.before) {
        Node n;
        n.id = next++;
        n.kind = NodeKind::Command;
        n.command = c;
        g.nodes.push_back(n);
        before.push_back(n.id);
    }
    const int firstStep = before.empty() ? sourceId : before.front();
    for (std::size_t i = 0; i + 1 < before.size(); ++i)
        g.edges.push_back({before[i], Port::Out, before[i + 1]});
    if (!before.empty())
        g.edges.push_back({before.back(), Port::Out, sourceId});

    // Gatilhos ligados ao primeiro passo.
    std::vector<int> triggers;
    for (const auto& t : job.schedule) {
        Node n;
        n.id = next++;
        n.kind = NodeKind::Trigger;
        n.trigger = t;
        g.nodes.push_back(n);
        triggers.push_back(n.id);
        g.edges.push_back({n.id, Port::Out, firstStep});
    }

    // Cofres: os da tarefa (na ordem dela) e depois os demais declarados.
    std::vector<int> vaults;
    auto addVault = [&](const std::string& id, bool linked) {
        Node n;
        n.id = next++;
        n.kind = NodeKind::Vault;
        n.vaultId = id;
        g.nodes.push_back(n);
        vaults.push_back(n.id);
        if (linked)
            g.edges.push_back({sourceId, Port::Out, n.id});
    };
    for (const auto& v : job.vaults)
        addVault(v, true);
    for (const auto& v : global.vaults)
        if (std::find(job.vaults.begin(), job.vaults.end(), v.id) == job.vaults.end())
            addVault(v.id, false);

    // Comandos depois: cada porta vira uma cadeia.
    std::vector<std::vector<int>> afterChains;
    auto addChain = [&](const std::vector<HookCommand>& cmds, Port port) {
        if (cmds.empty())
            return;
        std::vector<int> ids;
        for (const auto& c : cmds) {
            Node n;
            n.id = next++;
            n.kind = NodeKind::Command;
            n.command = c;
            g.nodes.push_back(n);
            ids.push_back(n.id);
        }
        g.edges.push_back({sourceId, port, ids.front()});
        for (std::size_t i = 0; i + 1 < ids.size(); ++i)
            g.edges.push_back({ids[i], Port::Out, ids[i + 1]});
        afterChains.push_back(ids);
    };
    addChain(job.hooks.after, Port::Out);
    addChain(job.hooks.onSuccess, Port::OnSuccess);
    addChain(job.hooks.onFailure, Port::OnFailure);

    // Disposição em colunas: gatilhos | antes... | origem | cofres e depois.
    const double sourceCol = static_cast<double>(before.size()) + 1.0;
    const std::size_t rightRows = vaults.size() + afterChains.size();
    const double rightHeight = std::max<double>(0, static_cast<double>(rightRows) - 1) * kRow;
    const double leftHeight = std::max<double>(0, static_cast<double>(triggers.size()) - 1) * kRow;
    const double centerY = std::max(rightHeight, leftHeight) / 2.0;

    for (std::size_t i = 0; i < triggers.size(); ++i) {
        Node* n = g.find(triggers[i]);
        n->x = 0;
        n->y = centerY - leftHeight / 2.0 + static_cast<double>(i) * kRow;
    }
    for (std::size_t i = 0; i < before.size(); ++i) {
        Node* n = g.find(before[i]);
        n->x = static_cast<double>(i + 1) * kColumn;
        n->y = centerY;
    }
    Node* s = g.find(sourceId);
    s->x = sourceCol * kColumn;
    s->y = centerY;
    double y = centerY - rightHeight / 2.0;
    for (int id : vaults) {
        Node* n = g.find(id);
        n->x = (sourceCol + 1) * kColumn;
        n->y = y;
        y += kRow;
    }
    for (const auto& chain : afterChains) {
        for (std::size_t i = 0; i < chain.size(); ++i) {
            Node* n = g.find(chain[i]);
            n->x = (sourceCol + 1 + static_cast<double>(i)) * kColumn;
            n->y = y;
        }
        y += kRow;
    }
    return g;
}

bool canConnect(const Graph& g, int from, Port port, int to, std::string* why)
{
    auto fail = [why](const char* msg) {
        if (why)
            *why = msg;
        return false;
    };
    const Node* a = g.find(from);
    const Node* b = g.find(to);
    if (!a || !b)
        return fail("bloco inexistente");
    if (from == to)
        return fail("um bloco não se liga a ele mesmo");
    for (const auto& e : g.edges)
        if (e.from == from && e.to == to)
            return fail("esses blocos já estão ligados");

    switch (a->kind) {
    case NodeKind::Vault:
        return fail("o cofre é o fim do caminho: ele não tem saída");
    case NodeKind::Trigger:
        if (port != Port::Out || !isCommandLike(b))
            return fail("um gatilho liga-se à origem ou a um comando que roda antes da cópia");
        break;
    case NodeKind::Command:
        if (port != Port::Out || !isCommandLike(b))
            return fail("um comando liga-se a outro comando ou à origem");
        for (const auto& e : g.edges)
            if (e.from == from && isCommandLike(g.find(e.to)))
                return fail("cada comando tem uma única saída: os comandos rodam em sequência");
        break;
    case NodeKind::Source:
        if (b->kind == NodeKind::Vault) {
            if (port != Port::Out)
                return fail("os cofres ligam-se à saída principal da origem");
        } else if (b->kind != NodeKind::Command) {
            return fail("a origem liga-se a cofres ou a comandos que rodam depois da cópia");
        }
        break;
    }

    if (b->kind == NodeKind::Trigger)
        return fail("um gatilho não tem entrada");
    if (b->kind == NodeKind::Command && a->kind != NodeKind::Trigger) {
        for (const auto& e : g.edges)
            if (e.to == to && isCommandLike(g.find(e.from)))
                return fail("este comando já recebe de outro bloco: os comandos rodam em sequência");
    }
    if (b->kind == NodeKind::Command && a->kind == NodeKind::Trigger) {
        // Gatilho só entra no começo da cadeia "antes".
        const Node* src = g.source();
        if (!src || !reaches(g, to, src->id))
            return fail("ligue o gatilho à origem ou a um comando que já leve até a origem");
    }
    if (b->kind == NodeKind::Source && a->kind == NodeKind::Command) {
        for (const auto& e : g.edges)
            if (e.to == to && g.find(e.from) && g.find(e.from)->kind == NodeKind::Command)
                return fail("a origem já recebe um comando: encadeie o novo comando antes dele");
    }
    if (reaches(g, to, from))
        return fail("essa ligação fecharia um ciclo");
    return true;
}

bool link(Graph& g, int from, Port port, int to, std::string* why)
{
    if (!canConnect(g, from, port, to, why))
        return false;
    g.edges.push_back({from, port, to});
    return true;
}

Conversion toJob(const Graph& g, const JobConfig& base)
{
    Conversion out;
    out.job = base;
    JobConfig& j = out.job;
    j.vaults.clear();
    j.schedule.clear();
    j.hooks = {};

    const Node* src = g.source();
    if (!src)
        return out;

    const std::vector<int> before = beforeChain(g, src->id);
    std::set<int> used{src->id};
    for (int id : before) {
        j.hooks.before.push_back(g.find(id)->command);
        used.insert(id);
    }

    // Gatilhos ligados à origem ou a qualquer comando da cadeia "antes",
    // na ordem de cima para baixo.
    std::vector<const Node*> triggers;
    for (const auto& n : g.nodes) {
        if (n.kind != NodeKind::Trigger)
            continue;
        for (const auto& e : g.edges)
            if (e.from == n.id && used.count(e.to)) {
                triggers.push_back(&n);
                break;
            }
    }
    std::stable_sort(triggers.begin(), triggers.end(), [](const Node* a, const Node* b) { return a->y < b->y; });
    for (const Node* t : triggers) {
        j.schedule.push_back(t->trigger);
        used.insert(t->id);
    }

    std::vector<const Node*> vaults;
    for (const auto& e : g.edges)
        if (e.from == src->id && e.port == Port::Out)
            if (const Node* n = g.find(e.to); n && n->kind == NodeKind::Vault)
                vaults.push_back(n);
    std::stable_sort(vaults.begin(), vaults.end(), [](const Node* a, const Node* b) { return a->y < b->y; });
    for (const Node* v : vaults)
        if (std::find(j.vaults.begin(), j.vaults.end(), v->vaultId) == j.vaults.end())
            j.vaults.push_back(v->vaultId);
    if (j.minVaultsOk > static_cast<int>(j.vaults.size()))
        j.minVaultsOk = 0; // "todos" continua valendo com menos cofres

    auto collect = [&](Port port, std::vector<HookCommand>& dst) {
        for (int id : afterChain(g, src->id, port)) {
            dst.push_back(g.find(id)->command);
            used.insert(id);
        }
    };
    collect(Port::Out, j.hooks.after);
    collect(Port::OnSuccess, j.hooks.onSuccess);
    collect(Port::OnFailure, j.hooks.onFailure);

    int looseTriggers = 0, looseCommands = 0;
    for (const auto& n : g.nodes) {
        if (used.count(n.id))
            continue;
        if (n.kind == NodeKind::Trigger)
            ++looseTriggers;
        else if (n.kind == NodeKind::Command)
            ++looseCommands;
    }
    if (looseTriggers)
        out.warnings.push_back(std::to_string(looseTriggers)
                               + " gatilho(s) sem ligação até a origem: não entram no agendamento");
    if (looseCommands)
        out.warnings.push_back(std::to_string(looseCommands)
                               + " comando(s) sem ligação com a origem: não serão executados");
    if (j.vaults.empty())
        out.warnings.push_back("nenhum cofre ligado à origem: a cópia não teria para onde ir");
    return out;
}

} // namespace synkro::config::flow
