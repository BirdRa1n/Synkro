# Synkro

Backup incremental, versionado e redundante para dados críticos no Windows, com foco em sistemas PDV: arquivos fiscais, configurações e bancos de dados (PostgreSQL, MySQL/MariaDB, Firebird, SQL Server, SQLite).

> **Estado:** as Fases 1 (motor e CLI), 2 (automação e bancos de dados), 3 (interface desktop) e 4 (robustez de campo e instalação) estão implementadas; a Fase 5 já trouxe as notificações externas, os cofres em rede e a cifragem do cofre. Faltam o instalador MSI assinado, a migração para MSVC e a validação em campo (PDVs e servidores de banco reais). Veja o [roadmap](docs/architecture.md#15-roadmap).

## Princípios

- Nunca destruir o único exemplar de um dado: exclusões na origem não apagam o backup.
- Cada disco de backup (**cofre**) é autossuficiente e fica num **disco físico diferente** da origem.
- Nada é considerado copiado sem verificação (SHA-256, gravação forçada em disco, troca atômica).
- Falha silenciosa é o pior defeito: backup atrasado, disco ausente ou cheio geram alerta.

## O que já funciona

| Recurso | Estado |
|---|---|
| Cofres com identidade (`vault.json`) e checagem do disco (disco físico, sistema de arquivos, espaço, teste de gravação) | ✅ |
| Backup incremental de arquivos com versões (`current/` + `versions/<run>/`), nada é apagado | ✅ |
| Vários cofres por tarefa, cada um independente (`min_vaults_ok`) | ✅ |
| Cofres em rede (NAS/servidor, `path: //servidor/pasta`) com credencial no cofre de segredos e sondagem do servidor | ✅ |
| Cofre cifrado (AES-256-GCM via CNG) com chave de recuperação; restauração em qualquer computador | ✅ |
| Cópia segura: SHA-256 durante a cópia, releitura sem cache opcional, troca atômica | ✅ |
| Guardas: origem ausente/vazia, exclusão e modificação em massa, arquivo-sentinela | ✅ |
| Diário de operações e recuperação de execuções interrompidas | ✅ |
| Retenção de versões e de arquivos excluídos | ✅ |
| Restauração point-in-time para pasta nova, conferida por SHA-256 | ✅ |
| Verificação do cofre (releitura sem cache) | ✅ |
| Configuração YAML com validação (arquivo/linha) e `config plan` em pt-BR | ✅ |
| Backup de bancos: PostgreSQL, MySQL/MariaDB, Firebird, SQL Server, SQLite e comando personalizado, com validação, guardas e retenção GFS | ✅ |
| Restauração de bancos sempre em banco/arquivo novo (`restore-db`) | ✅ |
| Cofre de segredos com DPAPI (`secret set`); senhas nunca no YAML nem na linha de comando | ✅ |
| Hooks antes/depois, espera por dependências (`depends_on`) | ✅ |
| Snapshot VSS da origem (requer administrador/SYSTEM) | ✅ |
| Agendador de Tarefas como SYSTEM, tarefa de manutenção e `config apply` (requer administrador) | ✅ |
| Estado local, alertas (backup atrasado, cofre ausente, pouco espaço, falhas) e Visualizador de Eventos | ✅ |
| Notificações por e-mail, webhook (Slack, Discord, Teams ou JSON) e Telegram, com fila, novas tentativas, lembretes e relatório diário (`notifications`, `notify`) | ✅ |
| Verificação periódica com reparo a partir de outro cofre (`maintenance --scrub`) | ✅ |
| Desligamento seguro após sucesso, com contagem regressiva e cancelamento | ✅ |
| Saúde dos discos (SMART via WMI) na checagem do cofre, nos alertas e no painel | ✅ |
| Trava no servidor contra dumps simultâneos de várias máquinas (`remote.server_lock`) | ✅ |
| Teste de restauração automático em banco temporário, com verificações SQL (`restore_test`, `restore-test`) | ✅ |
| Firebird (`gbak`) e SQL Server (`BACKUP ... COPY_ONLY, CHECKSUM`), com restauração e teste de restauração | ✅ |
| Exportar/importar a configuração entre PDVs, sem senhas nem identidade dos discos (desktop e `config export` / `config import`) | ✅ |
| Migração para MSVC | pendente |
| Interface desktop: painel, tarefas, cofres, histórico, restauração e configurações; tema claro/escuro/sistema; bandeja com alertas | ✅ |
| Assistente de tarefas com teste de conexão, editor YAML com validação ao vivo, cadastro de discos com checagem | ✅ |
| Pacote de instalação (`installer\package.ps1` + `install.ps1`): permissões, Visualizador de Eventos, menu Iniciar, Programas e Recursos | ✅ |
| Início automático na bandeja (instalador para todos os usuários; opção por usuário em Configurações) | ✅ |
| Teste de estresse (~100 mil arquivos, `SYNKRO_STRESS=1`) | ✅ |
| Instalador `Synkro-Setup.exe`, release automática pelo GitHub Actions e atualização automática no app (canal estável e beta) | ✅ |
| Instalador MSI assinado (depende do WiX e de um certificado de assinatura de código) | pendente |

## Documentação

| Documento | Conteúdo |
|---|---|
| [docs/architecture.md](docs/architecture.md) | Arquitetura, cofres, fluxo de backup, recuperação de falhas, roadmap |
| [docs/database-backup.md](docs/database-backup.md) | Backup consistente de bancos de dados |
| [docs/configuration.md](docs/configuration.md) | Configuração em YAML |
| [docs/operations.md](docs/operations.md) | Guia de operação: instalação, permissões, alertas, restauração, troca de disco |
| [docs/releases.md](docs/releases.md) | Versões, publicação na aba Releases (GitHub Actions) e atualização automática do app |
| [docs/pilot.md](docs/pilot.md) | Roteiro do piloto em PDVs: critérios, instalação, falhas provocadas, acompanhamento e avaliação |
| [config/examples/](config/examples) | YAMLs de exemplo (validados pelos testes) |

## Estrutura

```
apps/desktop/   synkro-desktop (Qt Widgets): telas, diálogos, tema e bandeja
apps/worker/    synkro-worker (CLI, executado pelo Agendador de Tarefas)
src/            libsynkro — motor (Qt Core + Win32, sem dependência de UI)
  catalog/      catálogo e manifests do cofre (JSON Lines)
  config/       carga, validação e descrição da configuração YAML
  core/         Result/Error, IDs de execução, pastas de dados, versão
  database/     drivers de banco, dumps, zstd, SQLite, retenção GFS e restauração
  engine/       backup, hooks, diário, recuperação, guardas, retenção, restauração, verificação
  hash/         SHA-256 via Windows CNG
  platform/     Win32: volumes, discos físicos, travas, VSS, Agendador de Tarefas, desligamento
  process/      execução de processos em Job Object com tempo limite
  secrets/      cofre de segredos (DPAPI)
  state/        banco local de estado e alertas
  scan/         varredura da origem, filtros e detecção de mudanças
  transfer/     cópia segura com verificação e troca atômica
  vault/        identidade, layout, localização e checagem de cofres
config/         exemplos de configuração
tests/          Qt Test (unit/ e integration/), integrados ao CTest
```

## Compilação

Requisitos: Windows 10/11, CMake ≥ 3.22, Ninja, Qt 6.5+ (kit MinGW 64-bit; a migração para MSVC está pendente) e **git no PATH** (o configure baixa o yaml-cpp 0.8.0 com FetchContent).

Pacote de instalação (Release + windeployqt + scripts): `powershell -ExecutionPolicy Bypass -File installer\package.ps1` — ver [operations.md §1](docs/operations.md#1-instalação).

```powershell
$env:SYNKRO_QT_DIR = "C:/Qt/6.12.0/mingw_64"
$env:PATH = "C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\Ninja;C:\Qt\Tools\CMake_64\bin;$env:PATH"

cmake --preset mingw-debug
cmake --build --preset mingw-debug
ctest --preset mingw-debug
```

No Qt Creator basta abrir o `CMakeLists.txt` da raiz.

## Synkro Desktop

`synkro-desktop.exe` é a interface para o dia a dia: mostra a situação dos backups, cadastra discos como cofres (com a checagem de disco físico, sistema de arquivos e gravação), cria tarefas pelo assistente, edita os YAMLs com validação ao vivo e o painel "O que esta tarefa vai fazer", gerencia segredos, executa tarefas e restaura arquivos ou bancos de qualquer cofre conectado — inclusive de outro computador.

- Sempre roda como administrador (pede o UAC ao abrir pelo menu Iniciar). No logon, a tarefa `\Synkro\Desktop` o abre na bandeja já elevado, sem pedido.
- Fechar a janela mantém o Synkro na bandeja; o ícone fica verde, amarelo ou vermelho conforme os alertas.
- `synkro-desktop --tray` inicia direto na bandeja.
- Cofres podem ser removidos da configuração (Cofres → Remover), com a opção de apagar os backups do disco digitando o nome do cofre.
- Arquivos YAML gerados pelo desktop podem ser editados à mão: a partir daí o desktop os abre no editor e nunca os regrava.

## Uso do worker

```powershell
# 1. Preparar o disco de backup (recusa disco físico igual ao da origem, FAT32, volume do Windows...)
synkro-worker vault check E:\ --source C:\PDV
synkro-worker vault init  E:\ --label "HD Externo 1" --source C:\PDV
synkro-worker vault show  E:\                      # mostra o vault_id para o synkro.yml
synkro-worker vault init  \\nas01\backup\pdv01 --label "NAS" --user nas01\synkro --password-secret nas-synkro
synkro-worker vault init  F:\ --label "HD Rodízio" --encrypt   # cofre cifrado: anote a chave de recuperação exibida
synkro-worker vault unlock F:\                     # PC reinstalado: guarda de novo a chave a partir da de recuperação

# 2. Configurar e conferir (C:\ProgramData\Synkro\config, ou --config-dir / SYNKRO_CONFIG_DIR)
synkro-worker secret set pg-pdv                    # senha do banco, digitada sem eco
synkro-worker config validate
synkro-worker config plan --job pdv-arquivos       # o que a tarefa vai fazer, em português
synkro-worker config apply                         # como administrador: registra as tarefas no Agendador

# 3. Executar (o Agendador faz isso sozinho depois do apply)
synkro-worker backup --job pdv-postgres
synkro-worker backup --job pdv-arquivos
synkro-worker backup --source C:\PDV --vault E:\ --job-id avulso   # sem configuração

# 4. Acompanhar
synkro-worker status                               # tarefas, cofres e alertas (sai com 2 se houver crítico)
synkro-worker maintenance --scrub                  # verificação dos cofres com reparo
synkro-worker notify test                          # como administrador: testa e-mail/webhook/Telegram
synkro-worker doctor                               # como administrador: este PDV está pronto?
synkro-worker diag                                 # relatório para o suporte (sem senhas)
synkro-worker notify list                          # o que foi avisado, tentativas e erros
synkro-worker runs  --vault E:\ --job pdv-arquivos
synkro-worker dumps --vault E:\ --job pdv-postgres

# 5. Recuperar
synkro-worker restore    --vault E:\ --job pdv-arquivos --to D:\Restaurado [--run r-...] [--path NFe/2026]
synkro-worker restore-db --vault E:\ --job pdv-postgres --database pdv_vendas --target-database pdv_vendas_restaurado
synkro-worker restore-test --job pdv-postgres              # restaura num banco temporário, confere e apaga

# 6. Levar a configuração para outro PDV (sem senhas e sem a identidade dos discos)
synkro-worker config export --to pdv-matriz.yml
synkro-worker config import --from pdv-matriz.yml --bind hd-externo-1=E: --bind hd-interno-2=F:
synkro-worker shutdown cancel                      # cancela um desligamento automático em contagem
```

A pasta de dados (`C:\ProgramData\Synkro`) pode ser trocada com `SYNKRO_DATA_DIR`, e `SYNKRO_NO_SHUTDOWN=1` desativa o desligamento automático (laboratório e manutenção).

Códigos de saída: `0` sucesso · `1` degradado · `2` falha · `3` bloqueado por guarda ou checagem · `4` tarefa já em execução · `64` uso incorreto.

## Licença

Veja [LICENSE](LICENSE).
