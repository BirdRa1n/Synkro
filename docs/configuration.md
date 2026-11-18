# Synkro — Configuração em YAML

> Complementa [architecture.md](architecture.md) e [database-backup.md](database-backup.md).

Tudo o que o Synkro faz — quais discos usa, o que copia, quando, como copia cada banco de dados, o que fazer antes e depois — fica descrito em **arquivos YAML** legíveis. A interface gráfica é um editor desses arquivos: o que se configura nela pode ser feito à mão no YAML, e vice-versa.

---

## 1. Princípios

1. **O YAML é a fonte da verdade da configuração.** O SQLite guarda apenas estado de execução, histórico e cache (o espelho da configuração é recriado a partir dos YAMLs).
2. **Um arquivo por tarefa.** Fácil de ler, copiar entre PDVs e versionar.
3. **Valores padrão sensatos.** Uma tarefa mínima tem poucas linhas; todo o resto tem padrão documentado.
4. **Nada inválido entra em vigor.** Toda alteração é validada antes de ser aplicada; se um arquivo estiver errado, o Synkro continua usando a última configuração válida e alerta.
5. **Sempre explicável.** `synkro-worker config plan` descreve em português, passo a passo, o que cada tarefa vai fazer.
6. **Sem segredos no YAML.** Senhas são referências ao cofre de segredos (DPAPI).

---

## 2. Onde ficam os arquivos

```
C:\ProgramData\Synkro\config\
├── synkro.yml                 configuração geral, cofres, alertas, padrões
├── jobs\
│   ├── pdv-arquivos.yml       uma tarefa por arquivo
│   ├── pdv-postgres.yml
│   ├── retaguarda-mysql.yml
│   └── caixa-sqlite.yml
├── schema\
│   └── synkro.schema.json     JSON Schema (validação e autocompletar)
└── .lastgood\                 cópia da última configuração válida (gerenciada pelo Synkro)
```

> **Estado da implementação:** validação (`config validate`, `config plan`), execução de tarefas `files` e `database`, `config apply` (com `.lastgood\` e registro no Agendador), a tarefa de manutenção, o cofre de segredos e os hooks já funcionam. Ainda pendentes: o JSON Schema para autocompletar em editores, a trava no servidor (`remote.server_lock`) e o teste de restauração automático (`restore_test`, Fase 4); o `config plan` avisa quando a tarefa usa algum deles. Chaves repetidas num mesmo bloco são recusadas.

**Segurança:** tarefas podem executar comandos, e o worker roda como **SYSTEM**. Por isso a pasta `config\` tem ACL de escrita **apenas para Administradores e SYSTEM**. O worker **recusa** carregar qualquer arquivo de configuração que usuários comuns possam modificar e emite alerta crítico — caso contrário, qualquer usuário do PDV poderia executar comandos como SYSTEM.

---

## 3. Como as alterações entram em vigor

```
Editar YAML (desktop ou editor de texto)
        │
        ▼
synkro-worker config validate      ← sintaxe + JSON Schema + regras de negócio
        │ válido
        ▼
synkro-worker config apply         ← atualiza tarefas do Task Scheduler, espelho no SQLite,
        │                             grava .lastgood\, envia job.yml (sem segredos) aos cofres
        ▼
Próxima execução usa a nova configuração
```

- O **desktop** faz validação + aplicação ao clicar em **Salvar**.
- Quem edita **à mão** pode rodar `config apply` ou simplesmente esperar: a tarefa de manutenção `\Synkro\Maintenance` (a cada 15 minutos e na inicialização do Windows) detecta mudanças pelo hash dos arquivos, valida e aplica.
- Se a validação falhar, nada muda: o Synkro continua com `.lastgood\` e abre o alerta **"Configuração inválida"** com o arquivo, a linha e o motivo.

### 3.1. Edição pelo desktop × edição manual

Bibliotecas de YAML não preservam comentários ao regravar um arquivo. Para não apagar anotações do usuário:

- arquivos criados pelo desktop recebem um cabeçalho `# Gerado pelo Synkro Desktop` e o hash do conteúdo gravado;
- se o desktop detectar que o arquivo foi **alterado à mão** (hash diferente), a tarefa abre no **editor YAML integrado** (com realce, validação ao digitar e o painel "O que esta tarefa vai fazer"), em vez do formulário — o arquivo nunca é regravado sem que o usuário veja o resultado.

### 3.2. Autocompletar em editores externos

Cada arquivo começa com:

```yaml
# yaml-language-server: $schema=../schema/synkro.schema.json
```

No VS Code (extensão YAML da Red Hat) e em outros editores compatíveis, isso dá autocompletar, documentação ao passar o mouse e marcação de erros.

---

## 4. Convenções

| Tipo | Formato | Exemplos |
|---|---|---|
| Duração | número + `s`, `m`, `h`, `d` | `30s`, `10m`, `2h`, `90d` |
| Tamanho | número + `KB`, `MB`, `GB`, `TB` | `500MB`, `2GB` |
| Horário | `HH:MM` (24 h, horário local) | `"22:00"` |
| Dias da semana | `mon tue wed thu fri sat sun` | `[mon, fri]` |
| Caminhos | barras `/` ou `\`; aspas recomendadas | `"C:/PDV/XML"` |
| Padrões de arquivo | estilo `.gitignore` (`*`, `**`, `?`), sem diferenciar maiúsculas | `"**/*.tmp"` |
| Segredos | `{ secret: <nome> }` ou `{ env: <VARIAVEL> }` | `{ secret: pg-pdv }` |
| Variáveis em comandos | `{job}`, `{run_id}`, `{date}`, `{output}`, `{staging}`, `{vault_path}` | `"--saida={output}"` |

IDs (`id:`) usam letras minúsculas, números e hífen, e não mudam depois de criados — são a ligação com o histórico e com as pastas nos cofres.

---

## 5. `synkro.yml` — configuração geral

```yaml
# yaml-language-server: $schema=schema/synkro.schema.json
version: 1

instance:
  name: "PDV Matriz"                  # aparece nos alertas e nos cofres

# Discos de destino. O cadastro inicial (verificações e criação do vault.json)
# é feito pelo desktop ou por `synkro-worker vault init`; aqui ficam as preferências.
vaults:
  - id: hd-externo-1
    vault_id: 7f1c9a2e-3b0d-4c7e-9a51-0d2b6e1f4a10   # gravado no vault.json do disco
    label: "HD Externo 1 (Seagate 2 TB)"
    rotation: true                    # disco que sai do local; ausência é esperada
    max_absence: 7d
    free_space_alert: 15%

  - id: hd-interno-2
    vault_id: 2b8e5d71-9c44-4f3a-8e0b-6a7c1d9f3e22
    label: "HD Interno 2 (WD 1 TB)"
    rotation: false
    max_absence: 1d
    path: "F:/"                       # opcional: onde procurar primeiro. Se o disco mudar de
                                      # letra, o Synkro o encontra pelo vault_id em qualquer volume

defaults:                             # valem para todas as tarefas, salvo se a tarefa sobrescrever
  vaults: [hd-externo-1, hd-interno-2]
  min_vaults_ok: all                  # all | número
  timeout: 4h
  schedule_catch_up: true             # executa assim que o PC ligar se perdeu o horário

alerts:
  no_success_for: 26h                 # alerta crítico se nenhuma tarefa teve sucesso nesse período
  tray: true
  windows_event_log: true             # grava alertas no Visualizador de Eventos (origem "Synkro")

maintenance:
  scrub:
    schedule: { weekly: { days: [sun], at: "03:00" } }
    sample: 10%                       # 10% dos arquivos por semana, em rodízio
    repair_from_other_vaults: true

shutdown:
  countdown: 120s
  allowed_after: [noite]              # grupos de tarefas que podem desligar o PC (ver job.group)

logging:
  level: info                         # debug | info | warn | error
  keep: 30d
```

---

## 6. Tarefa de arquivos (`type: files`)

### Mínima

```yaml
# yaml-language-server: $schema=../schema/synkro.schema.json
version: 1
id: pdv-arquivos
name: "Arquivos do PDV"
type: files

source: "C:/PDV"
schedule:
  - daily: "22:30"
```

Todo o resto (cofres, retenção, verificação, guardas) vem de `defaults` e dos padrões do Synkro.

### Completa

```yaml
# yaml-language-server: $schema=../schema/synkro.schema.json
version: 1
id: pdv-arquivos
name: "Arquivos do PDV (XML fiscais e configuração)"
type: files
group: noite
enabled: true

source: "C:/PDV"
include:
  - "XML/**"
  - "NFe/**"
  - "Config/**"
exclude:
  - "**/*.tmp"
  - "**/~$*"
  - "Logs/**"

vaults: [hd-externo-1, hd-interno-2]
min_vaults_ok: 1                      # sucesso com pelo menos 1 cofre; abaixo de "todos" = Degradado

schedule:
  - weekly: { days: [mon, tue, wed, thu, fri, sat], at: "22:30" }
  - on_startup: { delay: 10m }        # também ao ligar o PC, 10 min depois
depends_on: [pdv-postgres]            # começa depois do dump do banco
depends_mode: always                  # always | on_success

consistency:
  snapshot: vss                       # vss | none
  on_snapshot_failure: continue       # continue (copia sem snapshot, com aviso) | fail

change_detection:
  full_hash_every: 30                 # recalcula o hash de tudo a cada 30 execuções (0 = nunca)

verify: hash_on_copy                  # hash_on_copy | reread_uncached

retention:
  keep_versions: 90d
  keep_min_versions: 3
  keep_deleted: 180d

guards:
  max_deleted: 20%
  max_modified: 50%
  sentinel_file: "Config/pdv.ini"     # se sumir, a origem é considerada inválida

hooks:                                # comandos extras (executados como SYSTEM)
  before:
    - run: ["C:/PDV/bin/fechar-caixa.exe", "--silencioso"]
      timeout: 5m
      on_error: fail                  # fail | warn
  after:
    - run: ["C:/PDV/bin/abrir-caixa.exe"]
      on_error: warn
  on_failure: []
  on_success: []

shutdown_after: false
timeout: 6h
```

---

## 7. Tarefa de banco de dados (`type: database`)

Os detalhes de **como** cada motor é copiado estão em [database-backup.md](database-backup.md). No YAML o usuário escolhe o motor, a conexão, os bancos e, se quiser, ajusta o método.

### 7.1. PostgreSQL

```yaml
# yaml-language-server: $schema=../schema/synkro.schema.json
version: 1
id: pdv-postgres
name: "Banco PDV (PostgreSQL)"
type: database
group: noite

engine: postgresql
connection:
  host: localhost
  port: 5432
  user: synkro_backup
  password: { secret: pg-pdv }        # cadastrado com: synkro-worker secret set pg-pdv
  sslmode: prefer                     # disable | prefer | require | verify-full
  connect_timeout: 15s
  retries: 3

databases:                            # lista explícita, ou "all" (todos, menos templates)
  - name: pdv_vendas
  - name: pdv_estoque
    exclude_tables: ["public.log_*"]  # tabelas grandes que não precisam de backup

postgresql:
  format: custom                      # custom (recomendado) | plain | directory
  compress: 6
  include_globals: true               # papéis e tablespaces (pg_dumpall --globals-only)
  jobs: 1                             # >1 somente com format: directory
  extra_args: ["--no-comments"]

tools:
  pg_dump: auto                       # auto | caminho completo do executável

staging: local                        # local | first_vault
schedule:
  - daily: "22:00"
timeout: 2h

retention:
  keep_last: 7
  keep_daily: 14
  keep_weekly: 8
  keep_monthly: 12

guards:
  on_empty_database: block            # block | warn
  on_missing_tables: warn             # block | warn
  min_size_ratio: 0.5                 # alerta se o dump for menor que 50% do anterior

restore_test:
  enabled: true
  schedule: { weekly: { days: [sun], at: "04:00" } }
  connection: same                    # same | { host, port, user, password }
  exact_count_max_rows: 1000000
  checks:
    - name: "Existem vendas no último dia"
      sql: "SELECT count(*) > 0 FROM vendas WHERE data >= current_date - 1"
```

### 7.2. MySQL / MariaDB

```yaml
version: 1
id: retaguarda-mysql
name: "Retaguarda (MySQL)"
type: database
group: noite

engine: mysql                         # mysql | mariadb
connection:
  host: 127.0.0.1
  port: 3306
  user: synkro_backup
  password: { secret: mysql-retaguarda }

databases: [retaguarda, fiscal]

mysql:
  single_transaction: true
  routines: true
  triggers: true
  events: true
  myisam_strategy: warn               # warn | lock (usa --lock-tables se houver MyISAM)
  compression: zstd
  compression_level: 3
  extra_args: []

schedule:
  - daily: "22:10"
retention:
  keep_last: 7
  keep_daily: 14
  keep_monthly: 6
```

### 7.3. SQLite

```yaml
version: 1
id: caixa-sqlite
name: "Bancos locais do caixa (SQLite)"
type: database

engine: sqlite
sqlite:
  files:
    - "C:/PDV/dados/caixa.db"
    - "C:/PDV/dados/cache/*.db"       # curinga permitido
  integrity_check: true
  compression: zstd
  busy_timeout: 30s

schedule:
  - every: 2h                         # a cada 2 horas
    between: ["08:00", "22:00"]
retention:
  keep_last: 24
  keep_daily: 30
```

### 7.4. Comando personalizado

```yaml
version: 1
id: erp-exportacao
name: "Exportação do ERP do fornecedor"
type: database

engine: custom
custom:
  command: ["C:/ERP/bin/backup.exe", "/saida:{output}", "/silencioso"]
  output: file                        # file | stdout
  output_name: "erp.bak"
  success_exit_codes: [0]
  env:
    ERP_SENHA: { secret: erp-admin }  # entregue somente ao processo filho
  validate:
    command: ["C:/ERP/bin/backup.exe", "/verificar:{output}"]
  restore:
    command: ["C:/ERP/bin/backup.exe", "/restaurar:{input}", "/destino:{target}"]
  compression: zstd

schedule:
  - daily: "21:45"
```

### 7.5. Banco em outro servidor

Mesmo formato das seções 7.1 e 7.2, mudando a conexão. Os cenários e cuidados estão em [database-backup.md §4A](database-backup.md#4a-banco-na-própria-máquina--banco-em-outro-servidor).

```yaml
version: 1
id: servidor-postgres
name: "Banco central (PostgreSQL no servidor)"
type: database
group: noite

engine: postgresql
connection:
  host: 192.168.0.10                  # ou nome: srv-banco.empresa.local
  port: 5432
  user: synkro_backup
  password: { secret: pg-servidor }
  sslmode: verify-full                # padrão para host remoto: require
  ssl_ca: "C:/ProgramData/Synkro/certs/srv-banco-ca.pem"
  connect_timeout: 30s
  retries: 5                          # rede: mais tentativas
dump_retries: 1                       # refaz o dump inteiro se a conexão cair no meio

databases: all

remote:
  server_lock: true                   # trava no servidor: só uma máquina faz o dump por vez

schedule:
  - daily: "23:00"
timeout: 4h

restore_test:
  enabled: true
  schedule: { weekly: { days: [sun], at: "05:00" } }
  connection:                         # servidor de teste, não o de produção
    host: localhost
    port: 5432
    user: synkro_verify
    password: { secret: pg-teste-local }
```

Para MySQL/MariaDB os campos equivalentes são `ssl_mode` (`disabled` \| `preferred` \| `required` \| `verify_ca` \| `verify_identity`) e `ssl_ca`.

Regras do validador para host remoto:

- conexão sem criptografia exige `allow_insecure: true` (e gera aviso permanente);
- `engine: sqlite` com caminho de rede é rejeitado;
- aviso se `restore_test.connection` for `same` com host remoto.

### 7.6. `extra_args`: o que o usuário pode e não pode alterar

`extra_args` permite ajustar a ferramenta de dump sem esperar uma nova versão do Synkro. Para não quebrar a segurança nem a validação, o validador **rejeita** parâmetros que o Synkro controla:

| Motor | Parâmetros proibidos em `extra_args` |
|---|---|
| PostgreSQL | `--file`, `-f`, `--format`, `-F`, `--password`, `-W`, `--host`, `--port`, `--username`, `--dbname` |
| MySQL/MariaDB | `--result-file`, `-r`, `--password`, `-p`, `--defaults-file`, `--defaults-extra-file`, `--host`, `--user`, `--databases`, `--all-databases` |

Esses aspectos são configurados pelos campos próprios (`connection`, `format`...).

---

## 8. Agendamentos (`schedule`)

Uma tarefa pode ter vários gatilhos:

```yaml
schedule:
  - daily: "22:00"
  - weekly: { days: [sat, sun], at: "12:00" }
  - monthly: { day: 1, at: "02:00" }          # day: 1..28 ou last
  - every: 2h
    between: ["08:00", "22:00"]
    days: [mon, tue, wed, thu, fri]
  - on_startup: { delay: 10m }
  - on_logon: { delay: 5m }
  - manual                                     # sem agendamento; só sob demanda
```

Cada gatilho vira um gatilho da tarefa correspondente no Task Scheduler (`\Synkro\<id>`), com "executar assim que possível se perdido" conforme `schedule_catch_up`.

---

## 9. Referência de campos

### 9.1. Comuns a todas as tarefas

| Campo | Obrigatório | Padrão | Descrição |
|---|---|---|---|
| `version` | sim | — | Versão do formato (atual: `1`) |
| `id` | sim | — | Identificador permanente |
| `name` | sim | — | Nome exibido |
| `type` | sim | — | `files` \| `database` |
| `enabled` | não | `true` | |
| `group` | não | — | Agrupa tarefas para desligamento e dependências |
| `vaults` | não | `defaults.vaults` | Cofres de destino |
| `min_vaults_ok` | não | `all` | Cofres necessários para Sucesso |
| `schedule` | não | `manual` | Seção 8 |
| `depends_on` / `depends_mode` | não | — / `always` | Ordem entre tarefas |
| `timeout` | não | `defaults.timeout` | Tempo máximo da execução |
| `hooks` | não | — | `before`, `after`, `on_success`, `on_failure` |
| `shutdown_after` | não | `false` | Desligar após Sucesso |

### 9.2. `type: files`

| Campo | Padrão | Descrição |
|---|---|---|
| `source` | — (obrigatório) | Pasta de origem |
| `include` / `exclude` | tudo / nada | Padrões estilo `.gitignore` |
| `consistency.snapshot` | `vss` | Snapshot VSS do volume |
| `consistency.on_snapshot_failure` | `continue` | |
| `change_detection.full_hash_every` | `0` | |
| `verify` | `hash_on_copy` | |
| `retention.keep_versions` / `keep_min_versions` / `keep_deleted` | `90d` / `3` / `180d` | |
| `guards.max_deleted` / `max_modified` / `sentinel_file` | `20%` / `50%` / — | |

### 9.3. `type: database`

| Campo | Padrão | Descrição |
|---|---|---|
| `engine` | — (obrigatório) | `postgresql` \| `mysql` \| `mariadb` \| `sqlite` \| `custom` (Fase 4: `firebird`, `sqlserver`) |
| `connection` | — | Obrigatório exceto para `sqlite` e `custom`. Campos: `host`, `port`, `user`, `password`, `sslmode`/`ssl_mode`, `ssl_ca`, `connect_timeout`, `retries`, `allow_insecure` |
| `dump_retries` | `1` | Refazer o dump se a conexão cair durante a execução |
| `remote.server_lock` | `true` se host remoto | Trava no servidor contra dumps simultâneos de várias máquinas |
| `databases` | — | Lista ou `all` |
| `postgresql` / `mysql` / `sqlite` / `custom` | ver seção 7 | Opções do motor |
| `tools.*` | `auto` | Caminho da ferramenta cliente |
| `staging` | `local` | Área de preparo |
| `retention.keep_last` / `keep_daily` / `keep_weekly` / `keep_monthly` | `7` / `14` / `8` / `12` | GFS |
| `guards.on_empty_database` / `on_missing_tables` / `min_size_ratio` | `block` / `warn` / `0.5` | |
| `restore_test` | desabilitado | Teste de restauração agendado |

O JSON Schema em `config\schema\synkro.schema.json` é a referência completa e é gerado a partir do mesmo código que valida a configuração, para nunca divergir desta documentação.

---

## 10. Validação

`synkro-worker config validate` verifica, nesta ordem:

1. **Sintaxe YAML** — com arquivo, linha e coluna do erro.
2. **JSON Schema** — campos desconhecidos, tipos errados, valores fora da lista.
3. **Regras de negócio:**
   - IDs únicos; `depends_on` sem ciclos;
   - cofres referenciados existem e estão inicializados;
   - cofres de uma tarefa estão em **discos físicos diferentes** entre si e da origem;
   - nenhuma senha em texto puro; todos os segredos referenciados existem;
   - `extra_args` sem parâmetros proibidos;
   - ferramentas de dump encontradas (aviso se ausentes na máquina atual);
   - ACL da pasta `config\` segura.

Exemplo de saída:

```
✗ jobs/pdv-postgres.yml:9:13  connection.password: senhas em texto puro não são permitidas.
                               Use { secret: <nome> } e cadastre com: synkro-worker secret set <nome>
✗ jobs/pdv-arquivos.yml:14:3  vaults: "hd-externo-1" e "hd-interno-2" estão no mesmo disco físico (Disco 1).
⚠ jobs/retaguarda-mysql.yml   mysqldump não encontrado nesta máquina; a tarefa falhará até ser instalado.

2 erros, 1 aviso. Configuração NÃO aplicada; o Synkro continua usando a última configuração válida.
```

---

## 11. `config plan`: o que a tarefa vai fazer

Para cada tarefa, o Synkro gera uma descrição em linguagem simples do que será executado. A mesma descrição aparece no desktop, no painel **"O que esta tarefa vai fazer"**, e é atualizada enquanto o usuário edita.

```
> synkro-worker config plan --job pdv-postgres

Tarefa "Banco PDV (PostgreSQL)" [pdv-postgres] — backup de banco de dados

QUANDO
  • Todos os dias às 22:00.
  • Se o computador estiver desligado nesse horário, executa assim que ligar.
  • Tempo máximo: 2 h.

O QUE SERÁ FEITO
  1. Conectar ao PostgreSQL em localhost:5432 como "synkro_backup"
     (senha do segredo "pg-pdv" — encontrado ✓). Até 3 tentativas.
  2. Conferir se o pg_dump instalado (16.4, C:/Program Files/PostgreSQL/16/bin)
     é compatível com a versão do servidor.
  3. Conferir espaço: ~180 MB estimados; C: tem 52 GB livres; cofres têm 1,2 TB e 640 GB.
  4. Para cada banco:
       • pdv_vendas   → pg_dump formato custom, compressão 6
       • pdv_estoque  → pg_dump formato custom, compressão 6, sem as tabelas public.log_*
  5. Exportar papéis e tablespaces (pg_dumpall --globals-only).
  6. Validar cada dump com pg_restore --list e conferir o número de tabelas.
  7. Gravar os dumps nos cofres:
       • HD Externo 1 (Seagate 2 TB) → X:\SynkroVault\jobs\pdv-postgres\dumps\
       • HD Interno 2 (WD 1 TB)      → Y:\SynkroVault\jobs\pdv-postgres\dumps\
     Sucesso exige os 2 cofres; com apenas 1 o resultado será "Degradado".
  8. Apagar dumps antigos mantendo: últimos 7, 1 por dia por 14 dias,
     1 por semana por 8 semanas, 1 por mês por 12 meses.

PROTEÇÕES
  • Se o banco estiver vazio, nada é gravado e um alerta crítico é aberto.
  • Se o dump ficar menor que 50% do anterior, alerta crítico e nenhum dump antigo é apagado.

DEPOIS
  • A tarefa "Arquivos do PDV" começa em seguida (depende desta).
  • Teste de restauração: domingos às 04:00, em banco temporário.
```

---

## 12. Exportar e importar

- `synkro-worker config export --out pdv-matriz.zip` — todos os YAMLs, **sem** segredos e sem `vault_id`s (que são específicos dos discos).
- `synkro-worker config import pdv-matriz.zip` — importa em outro PDV; o desktop pede para associar os cofres e cadastrar os segredos que faltam.

Útil para padronizar a configuração em várias lojas.
