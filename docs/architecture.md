# Synkro — Arquitetura do sistema (v2.0)

> Backup incremental, versionado e redundante para dados críticos (com foco em sistemas PDV) no Windows.
>
> Esta versão substitui a proposta v1.0. As correções em relação a ela estão resumidas na [seção 16](#16-o-que-mudou-em-relação-à-v10).
>
> Documentos relacionados:
> - [database-backup.md](database-backup.md) — backup de bancos de dados (PostgreSQL, MySQL/MariaDB, SQLite, comando personalizado)
> - [configuration.md](configuration.md) — configuração em YAML, validação e `config plan`
>
> O Synkro tem dois tipos de tarefa: **`files`** (pastas e arquivos, seções 4 a 9 deste documento) e **`database`** (dumps consistentes de bancos, em [database-backup.md](database-backup.md)). Ambos gravam nos mesmos cofres e usam o mesmo agendamento, alertas e restauração.

---

## 0. Princípios inegociáveis

O Synkro guarda dados fiscais e comerciais. Todas as decisões deste documento derivam destas regras, e qualquer funcionalidade que conflite com elas é rejeitada:

1. **Nunca destruir o único exemplar de um dado.** Exclusões e sobrescritas na origem nunca apagam dados no destino imediatamente; só a retenção expira versões, e nunca a última.
2. **O destino é autossuficiente.** Um disco de backup, conectado a outro computador sem o banco local, precisa bastar para restaurar tudo.
3. **Nada é "concluído" sem verificação.** Um arquivo só entra no catálogo depois de gravado, sincronizado no disco e com hash conferido.
4. **Falha silenciosa é o pior defeito possível.** Backup atrasado, disco ausente, disco cheio ou disco degradando geram alerta visível.
5. **Backup sem restauração testada não existe.** A restauração faz parte do MVP e dos testes automatizados.
6. **Redundância real.** Duas cópias no mesmo disco físico não são redundância; o Synkro impede essa configuração.
7. **Consistência antes de velocidade.** Arquivos em uso (bancos do PDV) são copiados a partir de snapshot (VSS) ou de dump do próprio banco, nunca "a quente" sem aviso.

---

## 1. Visão geral

```
                        ┌──────────────────────────┐
                        │  synkro-desktop.exe      │  Qt Widgets
                        │  (config + monitoramento │  roda como usuário;
                        │   + ícone na bandeja)    │  pede UAC para alterar config
                        └────────────┬─────────────┘
                                     │ lê/escreve config, lê status
                                     ▼
┌───────────────────┐     ┌──────────────────────────┐
│ Windows Task      │────▶│  synkro-worker.exe       │  Qt Core apenas
│ Scheduler         │     │  (conta SYSTEM, elevado) │  sem interface
└───────────────────┘     └────────────┬─────────────┘
                                       │ usa
                                       ▼
                        ┌──────────────────────────┐
                        │  libsynkro (motor)       │  biblioteca C++ estática
                        │  scanner · detector ·    │  sem dependência de UI
                        │  cópia · verificação ·   │
                        │  catálogo · restauração  │
                        └──┬──────────┬─────────┬──┘
                           │          │         │
                    ┌──────▼───┐ ┌────▼────┐ ┌──▼─────────────────┐
                    │ Origem   │ │ VSS     │ │ Cofres (N discos)  │
                    │ (NTFS)   │ │snapshot │ │ autossuficientes   │
                    └──────────┘ └─────────┘ └────────────────────┘
                           ▲
               ┌───────────┴──────────┐
               │ C:\ProgramData\Synkro│  SQLite local = config + cache
               │  synkro.db, logs     │  (reconstruível a partir dos cofres)
               └──────────────────────┘
```

| Componente | Responsabilidade | Executa como |
|---|---|---|
| `libsynkro` | Todo o motor: varredura, detecção de mudança, cópia, verificação, catálogo, retenção, restauração, verificação periódica, **drivers de banco de dados**, **carga e validação da configuração YAML** | Biblioteca |
| `synkro-worker.exe` | Executa tarefas (backup de arquivos, dump de bancos, verificação, retenção, restauração, `config validate/plan/apply`). Não tem UI. | SYSTEM, via Task Scheduler |
| `synkro-desktop.exe` | Cadastro de discos e tarefas (editor dos YAMLs), dashboard, histórico, restauração, alertas na bandeja | Usuário (UAC para alterar config) |
| Task Scheduler | Disparo das execuções agendadas, inclusive sem usuário logado; tarefa `\Synkro\Maintenance` a cada 15 min | Windows |
| `C:\ProgramData\Synkro\config\*.yml` | **Fonte da verdade da configuração** ([configuration.md](configuration.md)) | Arquivos (escrita só Administradores/SYSTEM) |

**Por que SYSTEM:** o VSS e a leitura de arquivos bloqueados exigem privilégio de administrador; SYSTEM também enxerga todos os discos locais independentemente de quem está logado. Na v1 os destinos são **discos locais/USB** (requisito do produto), o que elimina o problema de credenciais de rede. Compartilhamentos de rede ficam para a v2 (seção 15).

---

## 2. Stack tecnológica

| Camada | Escolha | Observação |
|---|---|---|
| Linguagem | C++20 | |
| Compilador | **MSVC** (recomendado) | VSS, Task Scheduler e WMI são APIs COM; o SDK do Windows com MSVC é o caminho mais seguro. O projeto atual usa MinGW — migrar antes da Fase 2. |
| Framework | Qt 6 LTS (6.8) | Motor e worker usam só **Qt Core**; desktop usa Qt Widgets |
| Banco local | `sqlite3` (amalgamation, vendorizado) | Modo WAL + `busy_timeout`; independente do Qt SQL para manter o motor portátil |
| Formato no cofre | JSON Lines + arquivos comuns | Legível sem o Synkro; nada de SQLite no destino |
| Hash | SHA-256 via **Windows CNG (BCrypt)** | Sem OpenSSL. Abstraído atrás de `IHasher` |
| Arquivos | **Win32 nativo** (`CreateFileW`, `FindFirstFileExW`, `GetFileInformationByHandleEx`, `MoveFileExW`, `FlushFileBuffers`) | `std::filesystem` apenas em utilitários; caminhos sempre com prefixo `\\?\` |
| Snapshot | VSS (`IVssBackupComponents`) | |
| Agendamento | Task Scheduler 2.0 COM (`ITaskService`) | |
| Saúde do disco | WMI `MSFT_PhysicalDisk` / `MSFT_StorageReliabilityCounter` | |
| Configuração | **YAML** via `yaml-cpp` + validação por JSON Schema (`json-schema-validator` sobre `nlohmann/json`) | Schema gerado a partir do mesmo código que valida |
| Compressão | **zstd** (`libzstd`) em fluxo | Dumps MySQL/SQLite e scripts globais |
| Segredos | **DPAPI** em escopo de máquina (`CryptProtectData`) | Senhas de banco; nunca no YAML |
| Processos externos | `CreateProcessW` dentro de **Job Object** (encerra a árvore em timeout/queda) | `pg_dump`, `mysqldump`, hooks |
| Logs | spdlog (arquivo rotativo) + eventos estruturados no SQLite | |
| Testes | GoogleTest + testes de injeção de falha | |
| Build | CMake + vcpkg (manifest mode) | |
| Instalador | WiX Toolset (MSI) | Binários **assinados** (Authenticode), para evitar bloqueio por antivírus/SmartScreen |

**Licença Qt:** se o produto for comercial e de código fechado, usar Qt sob LGPL com vinculação dinâmica (DLLs do Qt distribuídas separadamente) ou adquirir licença comercial. Decidir antes da primeira distribuição.

---

## 3. Discos de destino ("Cofres")

O usuário adiciona, no aplicativo, os discos do Windows que guardarão as cópias. Cada disco cadastrado vira um **Cofre Synkro**. Uma tarefa de backup grava em **um ou mais cofres** — é assim que se obtém a duplicidade dos dados.

### 3.1. Requisitos para um disco ser aceito como cofre

Ao adicionar um disco, o Synkro executa uma checagem. Itens **bloqueantes** impedem o cadastro; **avisos** exigem confirmação explícita.

| # | Verificação | Tipo | Motivo |
|---|---|---|---|
| 1 | **Disco físico diferente do disco de origem** de cada tarefa que usará o cofre (comparação via `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`) | Bloqueante | Outra partição do mesmo disco morre junto com ele |
| 2 | **Disco físico diferente dos demais cofres da mesma tarefa** | Bloqueante | Dois cofres no mesmo disco não são redundância |
| 3 | Não é o volume do sistema (`C:`) nem o volume da origem | Bloqueante | |
| 4 | Sistema de arquivos **NTFS** (recomendado) ou exFAT | NTFS ok · exFAT aviso · FAT32/outros bloqueante | FAT32 não suporta arquivos > 4 GB; exFAT não tem journaling e perde dados com mais facilidade quando o disco é removido sem ejetar |
| 5 | Espaço livre ≥ tamanho estimado da origem × fator de versões (padrão 1,5×) | Aviso | |
| 6 | Teste de escrita: grava, faz flush, relê sem cache e confere um arquivo de teste | Bloqueante | Detecta disco somente leitura, falso ou defeituoso |
| 7 | Saúde SMART/Storage Reliability sem alertas | Aviso | |
| 8 | Unidade removível? | Informativo | Habilita a política de "cofre rotativo" (3.4) |
| 9 | BitLocker ativo? | Informativo | Recomendado para discos que saem do local |

### 3.2. Identidade do cofre

**Letras de unidade não identificam discos** — um HD USB pode ser `E:` hoje e `F:` amanhã, e outro disco qualquer pode assumir `E:`. O Synkro identifica o cofre por:

1. **Arquivo de identidade** `\SynkroVault\vault.json`, gravado na inicialização, com `vault_id` (UUID) — é a identidade real.
2. **Volume GUID** (`\\?\Volume{...}\`) e número de série do volume — usados para localizar o disco rapidamente.

A cada execução, o worker localiza o cofre pelo GUID (ou varrendo volumes) e **confirma o `vault_id` lido do `vault.json`**. Se não bater, o cofre é tratado como ausente — nunca se grava num disco que não tenha a identidade esperada.

```json
{
  "format": "synkro-vault",
  "format_version": 1,
  "vault_id": "7f1c9a2e-...",
  "label": "HD Externo 1 - Seagate",
  "created_at": "2026-11-17T22:00:00Z",
  "created_by_host": "PDV-MATRIZ",
  "synkro_version": "1.0.0"
}
```

### 3.3. Política de redundância por tarefa

Cada tarefa define:

| Parâmetro | Padrão | Significado |
|---|---|---|
| `vaults` | — | Lista de cofres (1 a N) |
| `min_vaults_ok` | todos os cofres | Quantos cofres precisam concluir com sucesso para o run ser **Sucesso**. Abaixo disso, mas ≥ 1: **Degradado** (alerta). Zero: **Falha**. |
| `max_vault_absence_days` | 7 | Alerta se um cofre não recebe backup há mais que isso |

**Cada cofre é independente:** possui seu próprio catálogo, versões e estado. Não há "clone de disco para disco": todos os cofres são alimentados **a partir do mesmo snapshot VSS da origem**, garantindo que todos representem o mesmo instante. Um cofre ausente (USB desconectado) não impede os outros; quando retorna, ele é atualizado com o estado atual no próximo run.

### 3.4. Cofre rotativo (proteção contra ransomware e furto)

Um disco permanentemente conectado e gravável é criptografado junto com a origem num ataque de ransomware. Recomendação operacional suportada pelo produto:

- Pelo menos **um cofre removível** que é desconectado e guardado fora do PDV (ex.: dois HDs alternados semanalmente).
- O Synkro trata a ausência desse cofre como normal até `max_vault_absence_days`; depois, alerta.
- O dashboard exibe, para cada cofre: última vez visto, último backup bem-sucedido, espaço livre e saúde.

---

## 4. Estrutura do cofre (destino)

```
X:\SynkroVault\
│
├── vault.json                      identidade do cofre
├── vault.lock                      lock exclusivo durante gravação
│
└── jobs\
    └── <job-id>\
        ├── job.json                cópia da configuração da tarefa
        ├── catalog.jsonl           estado completo da última execução bem-sucedida
        ├── catalog.jsonl.sha256
        │
        ├── current\                espelho mais recente dos arquivos da origem
        │   ├── XML\
        │   ├── NFe\
        │   └── Database\
        │
        ├── versions\               versões anteriores e arquivos excluídos na origem
        │   ├── <run-id>\...        (mesma árvore relativa de current\)
        │   └── ...
        │
        ├── runs\
        │   ├── <run-id>.manifest.jsonl   o que mudou nesse run
        │   ├── <run-id>.journal.jsonl    diário de operações (recuperação)
        │   └── <run-id>.summary.json     totais, status, duração, erros
        │
        └── .tmp\                   arquivos em gravação (limpo a cada início)
```

Tarefas `database` usam, no lugar de `current\`/`versions\`/`catalog.jsonl`, a pasta `dumps\<banco>\` com arquivos imutáveis, um `.dump.json` de metadados por dump e o catálogo `dumps.jsonl` — ver [database-backup.md §5](database-backup.md#5-onde-os-dumps-ficam-nos-cofres). Em ambos os tipos, `job.json` passa a ser `job.yml` (cópia da configuração da tarefa, sem segredos).

### 4.1. Modelo de versões

- **Arquivo novo:** gravado em `current\`.
- **Arquivo modificado:** a versão antiga é **movida** (rename no mesmo volume, instantâneo) de `current\caminho` para `versions\<run-id>\caminho`; depois a nova versão é gravada em `current\`.
- **Arquivo excluído na origem:** é movido de `current\` para `versions\<run-id>\` e marcado como `deleted` no manifest. Não é apagado.
- **Retenção** (seção 9) é o único mecanismo que apaga algo, e só apaga de `versions\`.

Com o catálogo e os manifests é possível reconstruir **o estado exato da origem em qualquer run retido** (restauração "point-in-time").

### 4.2. Catálogo e manifests

Uma linha JSON por arquivo, legível em qualquer editor:

```json
{"path":"NFe/2026/11/35261112345678000190550010000012341000012345.xml","size":8421,"mtime":"2026-11-17T21:43:10.1234567Z","sha256":"9f2c...","file_id":"0x0000...","attrs":32,"state":"present","since_run":"r-20261117-2200"}
```

- `catalog.jsonl` é **regravado inteiro ao fim de cada run bem-sucedido**, via arquivo temporário + flush + rename atômico, com o arquivo `.sha256` ao lado.
- Os manifests de run registram apenas as mudanças (`added`, `modified`, `deleted`, `failed`, `skipped`).
- Sem o banco local, um cofre pode ser lido, verificado e restaurado só com esses arquivos.

---

## 5. Banco local (SQLite)

Local: `C:\ProgramData\Synkro\synkro.db` (ACL: SYSTEM e Administradores com escrita; Usuários com leitura).

Função: **estado de execução + cache + histórico**. A configuração vem dos arquivos YAML ([configuration.md](configuration.md)); as tabelas `vaults`, `jobs` e `job_vaults` abaixo são um **espelho** recriado a cada `config apply`, usado para consultas do desktop e chaves do histórico. O inventário por cofre pode ser reconstruído a partir do `catalog.jsonl` / `dumps.jsonl` de cada cofre (comando `synkro-worker rebuild-cache`).

Configuração: `PRAGMA journal_mode=WAL`, `synchronous=FULL`, `foreign_keys=ON`, `busy_timeout=10000`. Migrações versionadas (`schema_version`).

### Tabelas

**`vaults`** — discos cadastrados

| Campo | Tipo |
|---|---|
| id (vault_id) | TEXT PK |
| label | TEXT |
| volume_guid | TEXT |
| volume_serial | TEXT |
| physical_disk_id | TEXT |
| filesystem | TEXT |
| is_removable | INTEGER |
| last_seen_at | TEXT (UTC) |
| last_health | TEXT (JSON) |
| enabled | INTEGER |

**`jobs`** — tarefas

| Campo | Tipo |
|---|---|
| id | TEXT PK |
| name | TEXT |
| source_path | TEXT |
| include_rules / exclude_rules | TEXT (JSON) |
| use_vss | INTEGER |
| pre_command / post_command | TEXT |
| pre_command_timeout_s | INTEGER |
| schedule | TEXT (JSON) |
| min_vaults_ok | INTEGER |
| retention | TEXT (JSON) |
| guards | TEXT (JSON) — ver seção 7 |
| shutdown_after | INTEGER |
| verify_mode | TEXT (`hash_on_copy` \| `reread_uncached`) |
| enabled | INTEGER |
| created_at / updated_at | TEXT |

A tabela `jobs` ganha também `type` (`files` \| `database`), `engine`, `config_file`, `config_hash` e `config_json` (configuração efetiva já validada, com padrões aplicados). Campos específicos de tarefas `files` só são preenchidos nesse tipo.

**`job_vaults`** — (job_id, vault_id), N:N

**`dumps`** — cache do catálogo de dumps de cada cofre

| Campo | Tipo |
|---|---|
| vault_id, job_id, run_id | TEXT |
| database_name | TEXT |
| engine | TEXT |
| relative_path | TEXT |
| size | INTEGER |
| sha256 | BLOB |
| server_version / tool_version | TEXT |
| validation_result | TEXT |
| restore_tested_at | TEXT |
| created_at | TEXT |

**`config_state`** — hash e data da última configuração aplicada, erros de validação pendentes.

**`runs`** — uma execução de uma tarefa

| Campo | Tipo |
|---|---|
| id | TEXT PK |
| job_id | TEXT |
| trigger | TEXT (`schedule` \| `manual` \| `startup` \| `catchup`) |
| started_at / finished_at | TEXT |
| status | TEXT (`running` \| `success` \| `degraded` \| `failed` \| `interrupted` \| `blocked_by_guard`) |
| snapshot_used | INTEGER |
| files_scanned | INTEGER |
| error_summary | TEXT |

**`run_vaults`** — resultado do run em cada cofre

| Campo | Tipo |
|---|---|
| run_id, vault_id | PK composta |
| status | TEXT |
| files_added / modified / deleted / failed | INTEGER |
| bytes_written | INTEGER |
| error_message | TEXT |

**`vault_files`** — cache do catálogo de cada cofre

| Campo | Tipo |
|---|---|
| vault_id, job_id | TEXT |
| relative_path | TEXT **COLLATE NOCASE** |
| size | INTEGER |
| mtime | INTEGER (FILETIME, 100 ns, UTC) |
| change_time | INTEGER |
| file_id | BLOB (128 bits, `FILE_ID_INFO`) |
| sha256 | BLOB |
| state | TEXT |
| since_run | TEXT |

Índices: `UNIQUE(vault_id, job_id, relative_path)`, `(vault_id, job_id, file_id)`.

**`events`** — eventos estruturados (arquivo copiado, erro de leitura, cofre ausente, alerta disparado...), com `run_id`, `vault_id`, `level`, `code`, `path`, `message`, `at`.

**`alerts`** — alertas ativos e histórico (`code`, `severity`, `subject`, `opened_at`, `acknowledged_at`, `resolved_at`).

**Concorrência entre processos:** worker e desktop acessam o mesmo banco. Dentro do worker, um único thread grava no SQLite (fila de escrita), em transações por lote. Entre processos, o WAL e o `busy_timeout` resolvem; o desktop só grava configuração.

---

## 6. Fluxo de um backup

```
 1. Adquirir lock da tarefa (mutex nomeado Global\Synkro.Job.<id>)
      └─ já em execução? → registra "ignorado: em andamento" e sai
 2. Recuperação: run anterior "running"/"interrupted"? → executar recuperação (seção 8)
 3. Validar origem (guardas de origem, seção 7)
 4. Localizar cofres pela identidade (vault.json) → lista de cofres disponíveis
      └─ nenhum disponível → Falha + alerta
 5. Executar hooks.before (se houver, ver configuration.md), com timeout
      └─ falhou → Falha (padrão) ou continuar com aviso (configurável)
 6. Criar snapshot VSS do volume de origem (se use_vss)
      └─ falhou → copiar sem snapshot, marcar arquivos bloqueados/abertos como "risco"
 7. Varredura da origem (no snapshot): FindFirstFileExW + FILE_ID_INFO
 8. Para cada cofre disponível (em paralelo, um thread de escrita por disco físico):
      a. Lock do cofre (vault.lock)
      b. Limpar .tmp\
      c. Comparar varredura × vault_files → lista de ações
      d. Aplicar guardas de mudança em massa (seção 7)
      e. Executar ações, registrando cada uma no journal.jsonl (flush por lote)
      f. Gravar manifest do run, regravar catalog.jsonl (atômico)
      g. Atualizar vault_files e run_vaults em transação
 9. Liberar snapshot VSS, executar hooks.after
10. Status do run conforme min_vaults_ok → alertas
11. Retenção (somente após status Sucesso)
12. Desligamento (se configurado e permitido, seção 10)
```

### 6.1. Detecção de mudança

A comparação é **origem × inventário registrado** (não origem × arquivo no destino). Por isso a precisão de data do sistema de arquivos do cofre não gera falsas modificações.

Um arquivo é considerado **modificado** se qualquer um mudar: `size`, `mtime`, `change_time` (`FILE_BASIC_INFO.ChangeTime`, que pega alterações de mtime feitas por software) ou `file_id` (arquivo substituído). Arquivos com metadados idênticos são ignorados, exceto:

- **Política "paranoica" por tarefa** (opcional): recalcular o hash de todos os arquivos a cada N execuções, para detectar alterações que preservam metadados.

O USN Change Journal **não** faz parte da v1: `FindFirstFileExW` com `FIND_FIRST_EX_LARGE_FETCH` varre centenas de milhares de arquivos em segundos. O USN será reavaliado com medições reais (Fase 5).

### 6.2. Cópia segura de um arquivo

```
1. Abrir origem (no snapshot) com FILE_FLAG_SEQUENTIAL_SCAN
2. Criar .tmp\<uuid> no cofre
3. Loop: ler bloco → atualizar SHA-256 → gravar bloco
4. FlushFileBuffers(temp); fechar
5. Verificação:
     hash_on_copy     → compara tamanho gravado; hash já calculado da origem
     reread_uncached  → reabre o temp com FILE_FLAG_NO_BUFFERING, recalcula SHA-256, compara
6. Se modificado: MoveFileExW(current\x → versions\<run>\x)
7. MoveFileExW(.tmp\<uuid> → current\x, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)
8. Preservar timestamps e atributos
9. Registrar no journal e no lote de escrita do catálogo
```

Somente após o passo 9 o arquivo conta como copiado. O padrão de `verify_mode` é `reread_uncached` para pastas de banco de dados e `hash_on_copy` para o restante.

### 6.3. Arquivos em uso e bancos do PDV

Em ordem de preferência:

1. **Tarefa do tipo `database`** ([database-backup.md](database-backup.md)) — dump consistente gerado pelo próprio banco (PostgreSQL, MySQL/MariaDB, SQLite; Firebird e SQL Server na Fase 4; ou comando personalizado). É o único método que garante consistência transacional. Em tarefas `files`, pastas de dados de bancos detectadas são excluídas automaticamente, com aviso.
2. **Snapshot VSS** — consistência de "falta de energia" (crash-consistent); bancos com VSS writer (SQL Server) ficam consistentes de fato.
3. **Sem snapshot** — o arquivo é copiado se puder ser aberto; se estava sendo escrito durante a cópia (tamanho/mtime mudaram entre o início e o fim), a cópia é descartada e o arquivo é marcado como `failed: changed_during_copy`.

O assistente de criação de tarefa detecta extensões comuns de banco (`.fdb`, `.gdb`, `.mdf`, `.ldf`, `.db`, `.sqlite`, `.mdb`, `.accdb`) e recomenda o método 1.

### 6.4. Casos especiais do sistema de arquivos

- Todos os caminhos com prefixo `\\?\` (suporte acima de 260 caracteres).
- Nomes comparados sem diferenciar maiúsculas/minúsculas (`COLLATE NOCASE`, normalização com `CompareStringOrdinal`).
- Links simbólicos e junções: **não seguidos** por padrão (evita laços e cópia fora da origem); registrados como aviso.
- Arquivos esparsos, ADS (alternate data streams) e ACLs: fora da v1, documentado.
- Disco removido durante a cópia (`ERROR_DEVICE_NOT_CONNECTED`, `ERROR_NOT_READY`): aborta **aquele cofre**, os demais continuam.
- Disco cheio (`ERROR_DISK_FULL`): aborta aquele cofre com alerta; nada já gravado é perdido.

---

## 7. Guardas de segurança

Proteções contra os cenários em que "o backup destrói o backup":

| Guarda | Regra padrão | Ação |
|---|---|---|
| Origem ausente | Pasta de origem não existe ou está inacessível | **Bloqueia** o run (nunca interpreta como "tudo foi excluído") |
| Origem vazia | 0 arquivos quando o último run teve > 0 | Bloqueia + alerta |
| Exclusão em massa | > 20% dos arquivos excluídos num run | Bloqueia + alerta; requer confirmação no desktop |
| Modificação em massa | > 50% dos arquivos modificados num run (indício de ransomware) | Bloqueia + alerta |
| Marcador de origem (opcional) | Arquivo-sentinela esperado na origem | Bloqueia se ausente |

Um run bloqueado tem status `blocked_by_guard`, não altera nenhum cofre e exige ação do usuário (confirmar ou investigar). Os limiares são configuráveis por tarefa.

---

## 8. Recuperação de falhas

| Falha | Comportamento |
|---|---|
| Worker morto / energia cai no meio do run | No próximo início, o run aparece como `running` sem processo vivo → marcado `interrupted`. O `journal.jsonl` daquele cofre é reprocessado: operações confirmadas são incorporadas ao catálogo; temporários em `.tmp\` são removidos. Em seguida, executa um run normal. |
| Arquivo parcialmente gravado | Só existe em `.tmp\` (nunca em `current\`). Removido. |
| Versão movida para `versions\` mas a nova não foi gravada | O journal indica a operação incompleta; a versão é restaurada para `current\` antes de prosseguir. |
| `catalog.jsonl` corrompido | Detectado pelo `.sha256`; reconstruído a partir do catálogo anterior + manifests, ou por varredura do próprio cofre. |
| Banco local perdido | `--rebuild-cache` reconstrói `vault_files` a partir dos cofres. |
| Execução agendada perdida (PC desligado) | Task Scheduler com `StartWhenAvailable` executa assim que o PC liga. |

O estado do último run bem-sucedido **nunca é sobrescrito antes de o novo estar completo**.

---

## 9. Retenção

Por tarefa:

| Parâmetro | Padrão |
|---|---|
| `keep_versions_days` | 90 |
| `keep_min_versions` | 3 por arquivo |
| `keep_deleted_days` | 180 (arquivos excluídos na origem) |
| `free_space_alert_pct` | 15% |

Regras:

- Só roda após um run com status **Sucesso**.
- Apaga apenas de `versions\`, nunca de `current\`.
- Nunca remove a última versão existente de um arquivo.
- Cada remoção é registrada no manifest do run.
- Sem espaço suficiente, o Synkro **alerta e falha**; nunca apaga versões além da política para abrir espaço.

---

## 10. Agendamento, execução e desligamento

### 10.1. Task Scheduler

O desktop cria/atualiza uma tarefa do Windows por tarefa Synkro (pasta `\Synkro\`), via COM:

- Conta **SYSTEM**, "executar com privilégios mais altos", executar com ou sem usuário logado.
- `StartWhenAvailable = true` (recupera execuções perdidas).
- `MultipleInstances = IgnoreNew` (reforço do mutex da tarefa).
- `ExecutionTimeLimit` configurável (padrão 12 h).
- `WakeToRun` opcional.
- Ação: `synkro-worker.exe backup --job <id> --trigger schedule`.

O módulo `scheduler` do Synkro **apenas traduz** a configuração para o Task Scheduler e confere periodicamente se as tarefas do Windows existem e estão corretas (autorreparo). Não existe timer próprio.

Além das tarefas de backup, existe a tarefa **`\Synkro\Maintenance`** (a cada 15 minutos e na inicialização do Windows), que:

- detecta alterações nos YAMLs, valida e aplica (ou alerta "Configuração inválida");
- recria/corrige tarefas do Task Scheduler ausentes ou alteradas;
- avalia alertas que dependem do tempo (backup atrasado, cofre ausente), que de outra forma só seriam percebidos quando uma tarefa rodasse;
- limpa temporários e arquivos de credenciais órfãos.

### 10.2. CLI do worker

```
synkro-worker backup   --job <id> [--trigger manual]
synkro-worker verify   --job <id> [--vault <id>] [--sample <pct>]
synkro-worker prune    --job <id>
synkro-worker restore  --job <id> --vault <id> --run <id|latest> --to <pasta> [--path <subcaminho>]
synkro-worker vault-check --vault <id>
synkro-worker vault init --disk <volume>
synkro-worker rebuild-cache

synkro-worker dump        --job <id>                     (equivale a backup para tarefas database)
synkro-worker restore-db  --job <id> --database <nome> --dump <id|latest> --target-database <novo>
synkro-worker restore-test --job <id>

synkro-worker config validate
synkro-worker config plan  [--job <id>]
synkro-worker config apply
synkro-worker config export --out <zip> | import <zip>
synkro-worker secret set <nome> | list | delete <nome>
synkro-worker maintenance
```

Códigos de saída documentados (0 sucesso, 1 degradado, 2 falha, 3 bloqueado por guarda, 4 já em execução).

### 10.3. Comunicação Desktop ↔ Worker

- **Estado e histórico:** via SQLite (WAL). O desktop consulta periodicamente.
- **Progresso em tempo real:** `QLocalServer` no worker (pipe nomeado com ACL permitindo leitura a Usuários), mensagens JSON de progresso. Se o desktop não estiver aberto, nada muda.
- **Execução manual pelo desktop:** dispara a tarefa existente no Task Scheduler (`IRegisteredTask::Run`), de modo que ela também roda como SYSTEM.

### 10.4. Desligamento após backup

Permitido somente se:

1. o run terminou com **Sucesso** (todos os `min_vaults_ok` atendidos);
2. nenhum guarda foi acionado;
3. não há outra tarefa Synkro em execução.

Fluxo: aviso na bandeja com contagem regressiva (padrão 120 s) e botão **Cancelar**; depois `InitiateShutdownW` com motivo planejado. Em caso de Degradado/Falha, o PC **não** desliga e o alerta fica visível para o próximo usuário.

---

## 11. Verificação periódica do cofre (scrub)

Tarefa agendada separada (padrão semanal) que relê arquivos do cofre **sem cache** e compara com o SHA-256 do catálogo.

- Modo amostral (padrão 10% por execução, rotacionando) ou completo.
- Divergência → alerta de **corrupção no cofre** com a lista de arquivos.
- **Reparo automático:** se outro cofre da mesma tarefa tiver o arquivo íntegro com o mesmo hash, ele é copiado para o cofre danificado. É um benefício direto da redundância.
- Também confere `vault.json`, `catalog.jsonl.sha256` e a saúde SMART.

---

## 12. Alertas e monitoramento

O Synkro **nunca falha em silêncio**. Alertas exibidos no dashboard, no ícone da bandeja (verde/amarelo/vermelho) e registrados em `alerts`:

| Alerta | Severidade |
|---|---|
| Nenhum backup bem-sucedido há mais de X horas (padrão 26 h) | Crítico |
| Run com status Falha ou Bloqueado por guarda | Crítico |
| Run Degradado (algum cofre não recebeu) | Atenção |
| Cofre ausente há mais de `max_vault_absence_days` | Atenção |
| Espaço livre abaixo do limite | Atenção → Crítico |
| Disco com alerta SMART | Crítico |
| Corrupção detectada na verificação | Crítico |
| Tarefa do Task Scheduler ausente/alterada | Crítico |

O desktop inicia com o Windows (bandeja). Notificações externas (e-mail/webhook) ficam para a Fase 5.

---

## 13. Restauração

Disponível desde a Fase 1 (CLI) e na interface a partir da Fase 3.

- **Escolha do ponto:** último run ou qualquer run retido (point-in-time).
- **Escopo:** tarefa inteira, pasta ou arquivos individuais.
- **Destino padrão: uma pasta nova.** Restaurar sobre a origem exige confirmação explícita e nunca sobrescreve sem antes mover o conteúdo atual para uma pasta de segurança.
- Cada arquivo restaurado tem o SHA-256 conferido contra o catálogo.
- **Modo "abrir cofre":** o desktop consegue abrir um cofre conectado em outro computador sem configuração prévia (lê `vault.json`, `job.json` e o catálogo) — cenário de PDV perdido.
- Como os arquivos ficam em pastas comuns, o usuário consegue copiar manualmente de `current\` mesmo sem o Synkro.

---

## 14. Estrutura do repositório

```
synkro/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── README.md
├── LICENSE
│
├── apps/
│   ├── desktop/                 Qt Widgets (toda a UI fica aqui)
│   │   ├── main.cpp
│   │   ├── MainWindow.{h,cpp,ui}
│   │   ├── dashboard/
│   │   ├── vaults/              cadastro e status de discos
│   │   ├── jobs/                assistente de tarefas (arquivos e bancos) + editor YAML integrado
│   │   ├── history/
│   │   ├── restore/
│   │   ├── settings/
│   │   └── tray/
│   └── worker/
│       └── main.cpp             CLI (Qt Core)
│
├── src/                         libsynkro (sem Qt Widgets)
│   ├── core/                    Job, Run, Result, Status, erros
│   ├── scan/                    Scanner, ChangeDetector, Rules (include/exclude)
│   ├── transfer/                CopyEngine, Verifier, VersionMover
│   ├── vault/                   Vault, VaultLocator, Catalog, Manifest, Journal, Retention, Scrubber
│   ├── restore/                 RestorePlanner, RestoreExecutor
│   ├── guards/                  SourceGuard, MassChangeGuard
│   ├── orchestrator/            BackupOrchestrator, JobLock, Recovery
│   ├── database/                IDatabaseDriver, PostgresDriver, MySqlDriver, SqliteDriver,
│   │                            CustomCommandDriver, DumpPipeline, DumpValidator, DumpCatalog,
│   │                            DumpRetention (GFS), RestoreTester, ToolLocator
│   ├── config/                  YamlLoader, SchemaValidator, RulesValidator, EffectiveConfig,
│   │                            PlanDescriber (config plan), ConfigApplier
│   ├── secrets/                 SecretStore (DPAPI), CredentialFile (PGPASSFILE / defaults-extra-file)
│   ├── process/                 ProcessRunner (Job Object, timeout, stdout em fluxo)
│   ├── storage/                 Database, migrações, repositórios
│   ├── alerts/
│   ├── ipc/                     ProgressServer / ProgressClient
│   ├── logging/
│   └── platform/
│       ├── interfaces/          IFileSystem, ISnapshotProvider, IHasher, IScheduler, IDiskInfo, IShutdown
│       └── windows/             Win32FileSystem, VssSnapshot, CngHasher, TaskScheduler, WmiDiskInfo, WindowsShutdown
│
├── config/
│   ├── schema/synkro.schema.json   gerado no build
│   └── examples/                   YAMLs de exemplo (arquivos, PostgreSQL, MySQL, SQLite)
│
├── installer/                   WiX
├── resources/                   ícones, traduções
│
├── tests/
│   ├── unit/
│   ├── integration/             backup → restauração → comparação de hashes
│   ├── fault/                   injeção de falhas
│   └── stress/
│
└── docs/
    ├── architecture.md          este documento
    ├── database-backup.md       backup de bancos de dados
    ├── configuration.md         referência do YAML
    ├── vault-format.md          especificação do formato do cofre (versionada)
    ├── database-schema.md
    ├── operations.md            guia para técnicos: rotação de discos, restauração
    └── development.md
```

`platform/interfaces` permite testar o motor com um sistema de arquivos simulado (injeção de falhas) e, no futuro, portar para outras plataformas.

### Estratégia de testes (obrigatória para aplicação crítica)

| Tipo | Exemplos |
|---|---|
| Ida e volta | Backup → restauração → todos os hashes iguais, em cada run de uma sequência com criação/modificação/exclusão |
| Injeção de falha | Matar o worker em pontos aleatórios da cópia (centenas de iterações); `IFileSystem` simulado retornando disco cheio, disco removido, erro de leitura, rename falhando |
| Guardas | Origem desmontada, origem esvaziada, 100% dos arquivos alterados |
| Sistema de arquivos | Caminhos > 260, Unicode, nomes que diferem só por maiúsculas, arquivos de 0 bytes e > 4 GB, arquivos bloqueados |
| Escala | 500 mil arquivos pequenos (perfil XML de NF-e); 50 GB em poucos arquivos grandes |
| Formato | Leitor de cofre independente, em script, valida que o cofre é legível sem o Synkro |

---

## 15. Roadmap

Cada fase tem **critério de saída**; nenhuma avança sem cumpri-lo.

### Fase 1 — Motor e CLI
Varredura, detecção, cópia segura, versões, catálogo/manifest/journal, recuperação de falhas, guardas, retenção, **restauração via CLI**, lock por tarefa, cofres com identidade (`vault.json`) e checagens da seção 3.1, múltiplos cofres por tarefa. **Configuração YAML** (carga, JSON Schema, regras de negócio, `config validate` e `config plan`).

**Saída:** testes de ida e volta e de injeção de falha passando; 500 mil arquivos processados; nenhum cenário de teste perde dados; todos os YAMLs de exemplo validam e geram `config plan` correto.

### Fase 2 — Automação, consistência e bancos de dados
Migração para MSVC, Task Scheduler (SYSTEM), tarefa de manutenção, `config apply`, VSS, hooks, alertas de backup atrasado e cofre ausente, desligamento seguro, `vault-check`. **Tarefas `database`:** PostgreSQL, MySQL/MariaDB, SQLite e comando personalizado; cofre de segredos DPAPI; validação e retenção GFS de dumps; `restore-db`.

**Saída:** uma semana rodando num PDV de teste, com desligamentos forçados e desconexão de disco, sem intervenção manual e sem perda; testes de ida e volta de cada motor passando, inclusive com gravações concorrentes; nenhuma senha encontrada em logs ou metadados.

### Fase 3 — Interface desktop
Dashboard, cadastro de cofres (com as checagens), assistente de tarefas de arquivos e de bancos (com teste de conexão), editor YAML integrado com validação ao digitar e painel "O que esta tarefa vai fazer", cadastro de segredos, histórico, restauração pela interface, bandeja com alertas, modo "abrir cofre".

**Saída:** um técnico configura um PDV do zero e faz uma restauração sem consultar documentação.

### Fase 4 — Robustez de campo
Scrub agendado com reparo entre cofres, saúde SMART, instalador MSI assinado, guia de operação, testes de estresse e de longa duração. Teste de restauração automático de dumps (`restore_test`); motores **Firebird** e **SQL Server**; exportar/importar configuração entre PDVs.

**Saída:** piloto em PDVs reais.

### Fase 5 — Evolução
Notificações externas (e-mail/webhook), compartilhamentos de rede (com credencial guardada via DPAPI), compressão/criptografia do cofre, deduplicação, USN Journal (se as medições justificarem), destino em nuvem com imutabilidade, outras plataformas.

---

## 16. O que mudou em relação à v1.0

| Problema na v1.0 | Correção na v2.0 |
|---|---|
| `current/` sobrescrito no lugar, embora a recomendação fosse ter versões | Modelo `current\` + `versions\<run>\` desde a Fase 1 (4.1) |
| Dois bancos sem fonte da verdade definida; SQLite no destino | Cofre autossuficiente em JSON Lines; SQLite local é config + cache reconstruível (4, 5) |
| VSS e bancos do PDV só na Fase 4 | `pre_command` com dump do banco e VSS na Fase 2; detecção de arquivos de banco no assistente (6.3) |
| Restauração e notificações só na Fase 5 | Restauração via CLI na Fase 1; alertas na Fase 2 (12, 13) |
| Destino identificado por caminho/letra de unidade | Identidade por `vault.json` + Volume GUID (3.2) |
| Sem redundância real | Múltiplos cofres por tarefa, em discos físicos distintos, verificados (3.1, 3.3) |
| Sem proteção contra origem ausente, exclusão em massa ou ransomware | Guardas (7) e cofre rotativo (3.4) |
| Verificação logo após a cópia lia o cache | Hash calculado durante a cópia + releitura sem cache + scrub periódico (6.2, 11) |
| Concorrência só dentro do processo | Mutex por tarefa, lock por cofre, WAL entre processos (5, 6) |
| Task Scheduler sem conta e sem recuperar execuções perdidas | SYSTEM, `StartWhenAvailable`, `IgnoreNew`, autorreparo (10.1) |
| Compartilhamento de rede na v1 sem tratar credenciais | Rede adiada para a Fase 5; v1 só discos locais/USB (1) |
| `std::filesystem`, OpenSSL, Qt SQL + sqlite3 | Win32 nativo, CNG, sqlite3 vendorizado (2) |
| Caminhos longos, maiúsculas/minúsculas, precisão de data | Tratados explicitamente (6.1, 6.4) |
| Duplicação `src/ui` × `apps/desktop`; scheduler com timer próprio | UI só em `apps/desktop`; scheduler apenas traduz para o Task Scheduler (10.1, 14) |
| Desligamento sem condições claras | Só após Sucesso, com contagem regressiva e cancelamento (10.4) |
| Roadmap sem critérios de saída | Critério de saída por fase (15) |

---

## 17. Decisões fixadas para a v1

| Decisão | Escolha |
|---|---|
| Plataforma | Windows 10/11 (x64) |
| Linguagem / framework | C++20, Qt 6.8 LTS (Core + Widgets) |
| Compilador | MSVC |
| Destinos | Discos locais e USB cadastrados como cofres; 1 a N por tarefa, em discos físicos distintos |
| Modelo de backup | Incremental com versões (`current\` + `versions\`) |
| Fonte da verdade | O cofre (JSON Lines); SQLite local é cache |
| Detecção | Metadados (size, mtime, change time, file ID) + hash opcional; varredura completa nativa |
| Consistência | Dump via `pre_command` > VSS > cópia direta com detecção de alteração |
| Exclusões na origem | Nunca apagam; vão para `versions\` e expiram pela retenção |
| Execução | Worker como SYSTEM via Task Scheduler |
| Desligamento | Opcional, somente após Sucesso |
| Restauração | CLI na Fase 1, interface na Fase 3, sempre para pasta nova por padrão |
| Bancos de dados | Tarefa `database` com dump lógico (PostgreSQL `pg_dump -Fc`, MySQL/MariaDB `mysqldump --single-transaction`, SQLite Online Backup API, comando personalizado); restauração sempre em banco novo |
| Configuração | YAML (um arquivo por tarefa) como fonte da verdade, validado por JSON Schema; desktop é editor dos YAMLs |
| Segredos | DPAPI escopo de máquina; proibido texto puro no YAML |

---

## 18. Pontos em aberto

1. **Requisitos adicionais para o disco de cofre** — a especificação do produto ("o disco selecionado para fazer os clones dos arquivos precisa...") está incompleta. A seção 3.1 traz os requisitos propostos; confirmar ou complementar.
2. ~~Bancos em servidores da rede~~ — **decidido em 2026-11-17:** PostgreSQL, MySQL/MariaDB e SQLite na Fase 2, Firebird e SQL Server na Fase 4. Os bancos podem estar na própria máquina ou num servidor separado; ambos os cenários são suportados desde a Fase 2 ([database-backup.md §4A](database-backup.md#4a-banco-na-própria-máquina--banco-em-outro-servidor)).
3. **Perfil de usuário** — quem configura (técnico ou o próprio lojista) define o nível de detalhe do assistente e se a configuração exige senha do Synkro além do UAC.
4. **Licenciamento do Qt** — LGPL ou comercial.
