#pragma once

#include "config/Config.h"

#include <string>
#include <vector>

namespace synkro::config::flow {

// Modelo do "modo fácil" do desktop: a tarefa vista como um fluxo de blocos
// ligados, no estilo dos editores de automação (docs/configuration.md §3.2).
//
//   [gatilho] ─┐
//   [gatilho] ─┼─> [comando antes] ─> [ORIGEM] ─saída──> [cofre]
//              └────────────────────>    │     ─saída──> [comando depois]
//                                        ├─sucesso─> [comando] ─> [comando]
//                                        └─falha───> [comando]
//
// O significado vem só das ligações: um cofre ligado à saída da origem entra
// em `vaults`; um gatilho ligado à origem (direto ou pela cadeia de comandos
// "antes") entra em `schedule`; comandos viram `hooks` conforme a porta de onde
// a cadeia sai. Sem dependência de Qt, para poder ser testado.

enum class NodeKind { Trigger, Command, Source, Vault };

// Portas de saída. A entrada de um bloco é única e não precisa de nome.
enum class Port { Out, OnSuccess, OnFailure };

struct Node {
    int id = 0;
    NodeKind kind = NodeKind::Trigger;
    ScheduleTrigger trigger{};   // Trigger
    HookCommand command{};       // Command
    std::string vaultId{};       // Vault (id do cofre em synkro.yml)
    double x = 0, y = 0;         // posição no quadro (só para a tela)
};

struct Edge {
    int from = 0;
    Port port = Port::Out;
    int to = 0;
    bool operator==(const Edge&) const = default;
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<Edge> edges;

    const Node* find(int id) const;
    Node* find(int id);
    const Node* source() const;
    int nextId() const;

    // Remove o bloco e as ligações dele. A origem não pode ser removida.
    bool removeNode(int id);
    void removeEdge(const Edge& e);
};

// Monta o fluxo de uma tarefa. Todos os cofres de `global` viram blocos (os
// que a tarefa usa já vêm ligados), para que acrescentar um disco seja só
// puxar uma linha. Já distribui os blocos em colunas.
Graph fromJob(const JobConfig& job, const GlobalConfig& global);

// Diz se a ligação pode ser feita (e por quê não). Regras:
//  - gatilho → comando ou origem;
//  - comando → comando ou origem (antes) ;
//  - origem.saída → cofre ou comando; origem.sucesso/falha → comando;
//  - cofre não tem saída; gatilho não tem entrada;
//  - um comando tem no máximo uma entrada vinda de comando/origem e uma saída;
//  - um comando "antes" (que leva à origem) não recebe nada da origem;
//  - sem ciclos e sem ligações repetidas.
bool canConnect(const Graph& g, int from, Port port, int to, std::string* why = nullptr);

// Acrescenta a ligação se canConnect permitir.
bool link(Graph& g, int from, Port port, int to, std::string* why = nullptr);

struct Conversion {
    JobConfig job;
    std::vector<std::string> warnings; // pt-BR: blocos soltos que serão ignorados
};

// Converte o fluxo de volta em tarefa. Parte de `base` (nome, origem,
// retenção etc. continuam como estão) e substitui vaults, schedule e hooks.
Conversion toJob(const Graph& g, const JobConfig& base);

} // namespace synkro::config::flow
