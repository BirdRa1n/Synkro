# Synkro

Backup incremental, versionado e redundante para dados críticos no Windows, com foco em sistemas PDV: arquivos fiscais, configurações e bancos de dados (PostgreSQL, MySQL/MariaDB, SQLite).

> **Estado:** em desenvolvimento. A Fase 1 (motor e CLI) está implementada; veja o [roadmap](docs/architecture.md#15-roadmap).

## Princípios

- Nunca destruir o único exemplar de um dado: exclusões na origem não apagam o backup.
- Cada disco de backup (**cofre**) é autossuficiente e fica num **disco físico diferente** da origem.
- Nada é considerado copiado sem verificação (SHA-256, gravação forçada em disco, troca atômica).
- Falha silenciosa é o pior defeito: backup atrasado, disco ausente ou cheio geram alerta.

## O que já funciona (Fase 1)

| Recurso | Estado |
|---|---|
| Cofres com identidade (`vault.json`) e checagem do disco (disco físico, sistema de arquivos, espaço, teste de gravação) | ✅ |
| Backup incremental de arquivos com versões (`current/` + `versions/<run>/`), nada é apagado | ✅ |
| Vários cofres por tarefa, cada um independente (`min_vaults_ok`) | ✅ |
| Cópia segura: SHA-256 durante a cópia, releitura sem cache opcional, troca atômica | ✅ |
| Guardas: origem ausente/vazia, exclusão e modificação em massa, arquivo-sentinela | ✅ |
| Diário de operações e recuperação de execuções interrompidas | ✅ |
| Retenção de versões e de arquivos excluídos | ✅ |
| Restauração point-in-time para pasta nova, conferida por SHA-256 | ✅ |
| Verificação do cofre (releitura sem cache) | ✅ |
| Configuração YAML com validação (arquivo/linha) e `config plan` em pt-BR | ✅ |
| Agendamento (Task Scheduler), VSS, hooks, alertas, desligamento | Fase 2 |
| Backup de bancos de dados (PostgreSQL, MySQL/MariaDB, SQLite) | Fase 2 — a configuração já é validada |
| Interface desktop | Fase 3 |

## Documentação

| Documento | Conteúdo |
|---|---|
| [docs/architecture.md](docs/architecture.md) | Arquitetura, cofres, fluxo de backup, recuperação de falhas, roadmap |
| [docs/database-backup.md](docs/database-backup.md) | Backup consistente de bancos de dados |
| [docs/configuration.md](docs/configuration.md) | Configuração em YAML |
| [config/examples/](config/examples) | YAMLs de exemplo (validados pelos testes) |

## Estrutura

```
apps/desktop/   synkro-desktop (Qt Widgets)
apps/worker/    synkro-worker (CLI, executado pelo Agendador de Tarefas)
src/            libsynkro — motor (Qt Core + Win32, sem dependência de UI)
  catalog/      catálogo e manifests do cofre (JSON Lines)
  config/       carga, validação e descrição da configuração YAML
  core/         Result/Error, IDs de execução, versão
  engine/       backup, diário, recuperação, guardas, retenção, restauração, verificação
  hash/         SHA-256 via Windows CNG
  platform/     utilitários Win32, volumes, discos físicos e travas
  scan/         varredura da origem, filtros e detecção de mudanças
  transfer/     cópia segura com verificação e troca atômica
  vault/        identidade, layout, localização e checagem de cofres
config/         exemplos de configuração
tests/          Qt Test (unit/ e integration/), integrados ao CTest
```

## Compilação

Requisitos: Windows 10/11, CMake ≥ 3.22, Ninja, Qt 6.5+ (kit MinGW 64-bit nesta fase; a migração para MSVC está prevista antes da Fase 2) e **git no PATH** (o configure baixa o yaml-cpp 0.8.0 com FetchContent).

```powershell
$env:SYNKRO_QT_DIR = "C:/Qt/6.12.0/mingw_64"
$env:PATH = "C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\Ninja;C:\Qt\Tools\CMake_64\bin;$env:PATH"

cmake --preset mingw-debug
cmake --build --preset mingw-debug
ctest --preset mingw-debug
```

No Qt Creator basta abrir o `CMakeLists.txt` da raiz.

## Uso do worker

```powershell
# 1. Preparar o disco de backup (recusa disco físico igual ao da origem, FAT32, volume do Windows...)
synkro-worker vault check E:\ --source C:\PDV
synkro-worker vault init  E:\ --label "HD Externo 1" --source C:\PDV
synkro-worker vault show  E:\                      # mostra o vault_id para o synkro.yml

# 2. Configurar e conferir
synkro-worker config validate                      # C:\ProgramData\Synkro\config ou --config-dir
synkro-worker config plan --job pdv-arquivos       # o que a tarefa vai fazer, em português

# 3. Executar
synkro-worker backup --job pdv-arquivos
synkro-worker backup --source C:\PDV --vault E:\ --job-id avulso   # sem configuração

# 4. Acompanhar e recuperar
synkro-worker runs    --vault E:\ --job pdv-arquivos
synkro-worker verify  --vault E:\ --job pdv-arquivos
synkro-worker restore --vault E:\ --job pdv-arquivos --to D:\Restaurado [--run r-...] [--path NFe/2026]
```

Códigos de saída: `0` sucesso · `1` degradado · `2` falha · `3` bloqueado por guarda ou checagem · `4` tarefa já em execução · `64` uso incorreto.

## Licença

Veja [LICENSE](LICENSE).
