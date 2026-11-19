# Synkro — Backup de bancos de dados

> Complementa [architecture.md](architecture.md). A configuração em YAML está em [configuration.md](configuration.md).

Este documento descreve **o que** o Synkro faz ao copiar um banco de dados, **como** faz em cada motor suportado, **onde** grava o resultado nos cofres (discos) e **como** restaurar.

---

## 1. Por que bancos não são copiados como arquivos

Copiar os arquivos de um banco em uso (`base/` do PostgreSQL, `ibdata1` do MySQL, `.db` do SQLite) gera uma cópia que **parece** correta e não abre, ou abre com dados corrompidos. Mesmo com VSS, o resultado só é "consistente como se tivesse faltado energia".

Por isso o Synkro tem um tipo de tarefa próprio, **`database`**, que pede ao próprio banco uma cópia **transacionalmente consistente** (dump lógico ou API de backup), valida essa cópia e só então a grava nos cofres.

Regra: em tarefas do tipo `files`, o Synkro **exclui automaticamente** pastas de dados de bancos detectadas (ex.: `PGDATA`, `datadir` do MySQL) e avisa o usuário para criar uma tarefa `database`.

---

## 2. Motores suportados

| Motor | Versão | Método | Ferramenta | Fase |
|---|---|---|---|---|
| PostgreSQL | 12+ | Dump lógico, formato *custom* | `pg_dump` / `pg_dumpall` | 2 |
| MySQL | 5.7, 8.x | Dump lógico SQL | `mysqldump` | 2 |
| MariaDB | 10.3+ | Dump lógico SQL | `mariadb-dump` (ou `mysqldump`) | 2 |
| SQLite | 3.x | Online Backup API (embutida no Synkro) | nenhuma | 2 |
| Firebird | 2.5, 3, 4, 5 | Backup lógico | `gbak` (e `isql` no teste de restauração) | 4 |
| SQL Server | 2016+ | `BACKUP DATABASE ... WITH COPY_ONLY, CHECKSUM` | `sqlcmd` | 4 |
| Personalizado | — | Comando definido pelo usuário | qualquer | 2 |

> **Estado da implementação:** todos os motores da tabela estão implementados — PostgreSQL (formatos `custom` e `plain`; `directory` ainda não é suportado), MySQL/MariaDB, Firebird, SQL Server, SQLite e comando personalizado —, com guardas, retenção GFS, `restore-db`, trava no servidor (§4A.3; PostgreSQL e MySQL/MariaDB) e teste de restauração automático (§9.1, `restore-test`). Os testes automatizados exercitam cada motor com ferramentas cliente simuladas; a validação contra servidores reais faz parte do critério de saída da Fase 4.

O Synkro **não embute** `pg_dump` nem `mysqldump`: usa as ferramentas cliente instaladas na máquina, localizadas automaticamente ou pelo caminho informado no YAML (ver 4.1).

---

## 3. O que acontece numa execução (visão geral)

Toda tarefa `database` segue o mesmo pipeline. Cada etapa é registrada em `events` e aparece no histórico.

```
 1. Lock da tarefa
 2. Resolver credenciais (cofre de segredos DPAPI) → arquivo temporário protegido
 3. Teste de conexão + leitura da versão do servidor
 4. Checagens prévias
      ├─ ferramenta cliente encontrada e compatível com a versão do servidor
      ├─ tamanho estimado do banco × espaço livre na área de preparo e nos cofres
      └─ permissões do usuário de backup
 5. Coletar "retrato" do banco (lista de tabelas + contagem estimada de linhas)
 6. Guardas (banco vazio? tabelas sumiram? — seção 7)
 7. Gerar o dump na área de preparo (staging)
      └─ saída da ferramenta → SHA-256 + compressão zstd em fluxo → arquivo
 8. Validar o dump (método específico do motor)
 9. Gravar o dump em cada cofre (motor de cópia: temp → flush → verificação → rename)
10. Gravar o arquivo de metadados (.dump.json) e atualizar o catálogo de dumps
11. Apagar arquivos temporários (credenciais e staging) — sempre, mesmo em falha
12. Retenção dos dumps antigos (somente após Sucesso)
13. (Opcional, agendado) Teste de restauração
```

### 3.1. Área de preparo (staging)

Por padrão o dump é gerado primeiro em `C:\ProgramData\Synkro\staging\<run-id>\` e depois copiado para os cofres. Motivos:

- a ferramenta gera **um** arquivo, e todos os cofres recebem **o mesmo** arquivo, com o mesmo hash;
- a validação é feita uma vez, antes de qualquer gravação nos cofres;
- se um cofre for desconectado durante a gravação, o dump não precisa ser refeito.

Se não houver espaço em `C:`, o YAML permite `staging: first_vault` (gera direto no primeiro cofre disponível e replica para os demais a partir dele, já validado).

### 3.2. Compressão

A saída é comprimida com **zstd** (nível 3 por padrão) **durante** a geração, sem arquivo intermediário. Exceção: o formato *custom* do PostgreSQL já é comprimido; nesse caso o Synkro grava sem recompressão (`compression: none` automático).

---

## 4. Como cada motor é copiado

### 4.1. Localização das ferramentas

Ordem de busca, por motor:

1. caminho explícito no YAML (`tools.pg_dump`, `tools.mysqldump`...);
2. registro do Windows (chaves de instalação do PostgreSQL / MySQL / MariaDB);
3. pastas padrão (`C:\Program Files\PostgreSQL\*\bin`, `C:\Program Files\MySQL\*\bin`, `C:\Program Files\MariaDB *\bin`), escolhendo a **versão mais alta**;
4. `PATH`.

O caminho e a versão da ferramenta usada ficam registrados nos metadados do dump.

### 4.2. PostgreSQL

**Comando executado** (um por banco listado na tarefa):

```
pg_dump --host=<host> --port=<port> --username=<user>
        --format=custom --compress=6
        --no-password --verbose
        [--schema=... / --exclude-table=... conforme YAML]
        --file=<staging>\<banco>.dump
        <banco>
```

Credencial: arquivo temporário apontado por `PGPASSFILE` (seção 6). Nunca pela linha de comando.

**Opcional — objetos globais** (`include_globals: true`): `pg_dumpall --globals-only --no-role-passwords` gera `globals.sql` com papéis e tablespaces, necessário para restaurar em um servidor novo.

**Consistência:** `pg_dump` abre uma transação `REPEATABLE READ` e exporta um snapshot único. O dump representa **um instante exato** do banco sem bloquear as gravações do PDV.

**Compatibilidade de versão:** `pg_dump` precisa ter versão principal **maior ou igual** à do servidor. O Synkro consulta `SHOW server_version_num` e compara com `pg_dump --version`; se for incompatível, a tarefa **falha antes de começar**, com mensagem indicando qual versão instalar.

**Validação:** `pg_restore --list <arquivo>` precisa ler o índice do arquivo inteiro sem erro, e a quantidade de entradas `TABLE DATA` deve bater com o número de tabelas do retrato (passo 5).

**Permissões mínimas recomendadas** (usuário dedicado):

```sql
CREATE ROLE synkro_backup LOGIN PASSWORD '...';
GRANT pg_read_all_data TO synkro_backup;      -- PostgreSQL 14+
-- Versões anteriores: GRANT SELECT em todas as tabelas/sequências de cada schema
```

### 4.3. MySQL / MariaDB

**Comando executado** (um por banco):

```
mysqldump --defaults-extra-file=<arquivo temporário de credenciais>
          --host=<host> --port=<port>
          --single-transaction --quick
          --routines --triggers --events
          --hex-blob --default-character-set=utf8mb4
          --set-gtid-purged=OFF                  (somente MySQL com GTID)
          --no-tablespaces
          <banco>
```

No MariaDB é usado `mariadb-dump` quando disponível (mesmos parâmetros, sem `--set-gtid-purged`).

- A saída vai para o **stdout**, que o Synkro lê em fluxo, calcula o SHA-256, comprime com zstd e grava em `<banco>.sql.zst`.
- O banco é exportado **sem** `--databases` (sem `CREATE DATABASE`/`USE`), para que possa ser restaurado com outro nome.
- `--defaults-extra-file` evita a senha na linha de comando (visível na lista de processos).

**Consistência:** `--single-transaction` garante um instante único **para tabelas InnoDB**. Antes do dump o Synkro consulta `information_schema.tables`:

- se houver tabelas **MyISAM/Aria**, emite aviso e, se `myisam_strategy: lock` estiver configurado, usa `--lock-tables` (bloqueia gravações durante o dump daquele banco); o padrão é `warn`.

**Validação:**

1. código de saída 0 e stderr sem linhas de erro;
2. a última linha do dump precisa ser `-- Dump completed on ...` (o `mysqldump` só a escreve ao terminar com sucesso);
3. o número de `CREATE TABLE` deve bater com o retrato.

**Permissões mínimas recomendadas:**

```sql
CREATE USER 'synkro_backup'@'localhost' IDENTIFIED BY '...';
GRANT SELECT, SHOW VIEW, TRIGGER, EVENT, LOCK TABLES, PROCESS ON *.* TO 'synkro_backup'@'localhost';
```

### 4.4. SQLite

Não usa ferramenta externa: o Synkro já embute a biblioteca `sqlite3`.

**Como faz:**

1. abre o arquivo de origem em modo somente leitura (`SQLITE_OPEN_READONLY`), com `busy_timeout`;
2. usa a **Online Backup API** (`sqlite3_backup_init` / `sqlite3_backup_step` em lotes de páginas) para copiar para `<staging>\<nome>.db`. A API reinicia automaticamente se a aplicação gravar durante a cópia, então o resultado é sempre consistente — inclusive com o banco em modo WAL;
3. executa `PRAGMA integrity_check` na **cópia** (precisa retornar `ok`);
4. comprime para `<nome>.db.zst`.

Vários arquivos podem ser listados numa mesma tarefa, inclusive com curinga (`C:/PDV/dados/*.db`).

### 4.5. Comando personalizado

Para motores não suportados ou procedimentos próprios do fornecedor do PDV:

```yaml
engine: custom
custom:
  command: ["C:/PDV/bin/exportar.exe", "--saida", "{output}"]
  output: file            # file = o comando grava em {output}; stdout = o Synkro lê a saída
  output_name: "pdv-export.bak"
  success_exit_codes: [0]
  validate:
    command: ["C:/PDV/bin/exportar.exe", "--verificar", "{output}"]
```

O Synkro trata o arquivo gerado como qualquer outro dump: hash, compressão, cópia verificada para os cofres, metadados e retenção.

### 4.6. Firebird

**Comando executado** (um por banco listado):

```
gbak -b -g -v -y <staging>\pdv.gbak.log -user SYSDBA -fetch_password <arquivo-temporário> localhost/3050:C:\PDV\dados\PDV.FDB <staging>\pdv.fbk
```

- `-b` é um backup lógico feito numa única transação: o PDV continua funcionando e o arquivo representa um instante único do banco.
- `-g` desliga a coleta de lixo durante o backup (mais rápido e sem efeito colateral no banco); `firebird.garbage_collect: true` a religa.
- A senha vai por **arquivo** (`-fetch_password`) na pasta privada da execução, nunca na linha de comando; o caminho desse arquivo também não vai para os metadados.
- `databases` recebe o **caminho do `.fdb` como o servidor o enxerga** (ou um alias do `databases.conf`); o nome lógico do dump é o nome do arquivo sem extensão, ou o `name` informado. `databases: all` não existe no Firebird.

**Validação:** código de saída 0 **e** a linha final do log verboso (`closing file, committing, and finishing`); as tabelas vêm das linhas `writing data for table` do mesmo log. Depois, compressão zstd (`.fbk.zst`).

**Restauração:** `gbak -c` num arquivo `.fdb` **novo** (`restore-db --to`); `-c` falha se o arquivo existir — o Synkro nunca usa `-rep`. No teste de restauração, o banco temporário é aberto com `isql` (usuário e senha por `ISC_USER`/`ISC_PASSWORD`, só no ambiente do processo filho) para conferir as tabelas e rodar as verificações, e depois é apagado com `DROP DATABASE`. Com servidor remoto, informe `firebird.restore_test_dir` (pasta **no servidor** para o banco temporário).

> A criptografia do tráfego do Firebird depende de `WireCrypt` no `firebird.conf` do servidor (padrão `Enabled` no 3+); o `config validate` avisa quando o servidor é remoto. A trava no servidor (§4A.3) não existe para o Firebird.

### 4.7. SQL Server

**O que é executado** (por banco):

```sql
-- 1. retrato das tabelas (para as guardas e o teste de restauração)
SELECT s.name + '.' + t.name FROM sys.tables t JOIN sys.schemas s ON ...
-- 2. backup sem interferir na cadeia de backups do DBA
BACKUP DATABASE [ERP] TO DISK = N'<backup_dir>\synkro_<run>_ERP.bak' WITH COPY_ONLY, CHECKSUM, INIT, FORMAT
-- 3. validação
RESTORE VERIFYONLY FROM DISK = N'<...>.bak' WITH CHECKSUM
```

- `COPY_ONLY` não interfere na cadeia de backups diferenciais e de log que o DBA já mantenha.
- `CHECKSUM` faz o SQL Server conferir cada página ao gravar; `RESTORE VERIFYONLY WITH CHECKSUM` relê o arquivo inteiro.
- **Quem grava o `.bak` é o serviço do SQL Server**, não o Synkro. Por isso:
  - **servidor local:** sem configuração, o Synkro usa a pasta de backup padrão da instância (`SERVERPROPERTY('InstanceDefaultBackupPath')`); `sqlserver.backup_dir` troca a pasta;
  - **servidor remoto:** `sqlserver.backup_dir` é obrigatório e precisa ser um compartilhamento (`\\servidor\synkro$`) em que a conta do serviço do SQL Server grave e a conta do Synkro nesta máquina (SYSTEM = a conta de computador `DOMINIO\PC$`) leia.
- Depois de validado, o `.bak` é copiado (com SHA-256) para o preparo e **apagado** da pasta do servidor. Por padrão o Synkro comprime com zstd (`.bak.zst`), o que funciona também no SQL Server Express; `native_compression: true` usa `WITH COMPRESSION` do próprio servidor.
- **Autenticação:** `auth: sql` com a senha em `SQLCMDPASSWORD` (só no ambiente do processo filho, nunca `-P`) ou `auth: windows` (`-E`, conta do serviço do Synkro). `encrypt: mandatory` (padrão em servidor remoto) liga a criptografia (`-N`); `trust_server_certificate: true` aceita certificado autoassinado (`-C`).
- Instância nomeada: `host: "PDV01\\SQLEXPRESS"`, sem `port`.

**Restauração:** confere que o banco de destino **não existe** (`DB_ID`), copia o `.bak` conferido para a pasta de backup do servidor, lê os arquivos lógicos (`RESTORE FILELISTONLY`) e executa `RESTORE DATABASE [novo] ... WITH CHECKSUM, MOVE ...` para as pastas de dados padrão da instância, com nomes novos — os arquivos do banco original nunca são tocados.

**Permissões mínimas recomendadas:** `db_backupoperator` em cada banco (backup) e, para o teste de restauração num servidor de teste, `dbcreator`.

---

## 4A. Banco na própria máquina × banco em outro servidor

Os dois cenários são suportados desde a Fase 2. A diferença está em **onde o Synkro roda** e **por onde os dados passam**.

### 4A.1. Cenários

| Cenário | Onde o Synkro roda | Como funciona |
|---|---|---|
| **A. Banco local** (`host: localhost`) | No próprio PDV | Dump local, gravado nos cofres do PDV |
| **B. Banco em servidor, dump remoto** | Numa estação (PDV ou máquina de retaguarda) | `pg_dump`/`mysqldump` rodam na estação, conectam ao servidor pela rede e recebem os dados; o arquivo é gravado nos cofres **da estação** |
| **C. Synkro instalado no servidor** | No próprio servidor (Windows) | Igual ao cenário A, com os cofres conectados ao servidor |

**Recomendação:**

- Servidor **Windows** em que é possível instalar software → **cenário C**. É o mais rápido e não depende da rede nem de uma estação ligada.
- Servidor **Linux**, servidor gerenciado (nuvem, hospedagem) ou sem permissão para instalar → **cenário B**. O Synkro v1 roda só no Windows, mas `pg_dump` e `mysqldump` funcionam normalmente contra servidores Linux.

**SQLite não tem cenário B.** SQLite é um arquivo, não um servidor: abrir um `.db` por compartilhamento de rede (SMB) para fazer backup é inseguro (locks não confiáveis). Se o arquivo SQLite estiver em outra máquina, o Synkro precisa estar instalado nela. O validador rejeita `sqlite.files` apontando para caminhos de rede (`\\servidor\...` ou unidades mapeadas).

Comandos `custom` sempre executam na máquina do Synkro; se o comando do fornecedor precisar rodar no servidor, use o cenário C.

### 4A.2. O que muda no dump remoto (cenário B)

| Aspecto | Como o Synkro trata |
|---|---|
| **Rede instável** | Até `connection.retries` tentativas de conexão com intervalo crescente. Se a conexão cair **durante** o dump, o arquivo parcial é descartado e o dump inteiro é refeito (até `dump_retries`, padrão 1). Nada parcial chega aos cofres. |
| **Servidor fora do ar** | Status **Falha** com motivo "servidor inacessível" — distinto de "banco vazio", para não acionar as guardas erradas. Alerta crítico. |
| **Criptografia em trânsito** | Padrão `sslmode: require` (PostgreSQL) / `ssl_mode: required` (MySQL) quando o host **não** é local. Desabilitar exige `allow_insecure: true` explícito e gera aviso permanente no dashboard. `verify-full` com `ssl_ca` é o recomendado. |
| **Versão da ferramenta** | A ferramenta cliente na estação precisa ser compatível com a versão do **servidor** (ex.: servidor PostgreSQL 16 exige `pg_dump` 16+ na estação). Verificado antes de cada dump. |
| **Tempo e volume** | O dump trafega pela rede. O `config plan` mostra o tamanho estimado do banco e um tempo estimado com base nas execuções anteriores. O `timeout` padrão sobe para 4 h quando o host é remoto. |
| **Permissões no servidor** | O usuário de backup precisa aceitar conexão a partir do IP da estação (`pg_hba.conf` no PostgreSQL; `'synkro_backup'@'<ip>'` no MySQL). Firewall do servidor liberado só para esse IP, na porta do banco. |
| **Metadados** | O `.dump.json` registra `host`, `port` e o nome da máquina que fez o dump. |

### 4A.3. Várias estações apontando para o mesmo servidor

Em empresas com vários PDVs e um servidor central, **apenas uma máquina** deve fazer o dump de cada banco — senão o mesmo banco é exportado várias vezes por noite, sobrecarregando o servidor.

- O assistente pergunta qual máquina será a responsável pelo backup do servidor; nas demais, a tarefa não é criada.
- **Proteção extra:** antes do dump, o Synkro obtém uma trava **no próprio servidor de banco** (`pg_try_advisory_lock(hashtext('synkro:<banco>'))` no PostgreSQL, dentro do próprio banco; `GET_LOCK('synkro:<banco>', 0)` no MySQL/MariaDB). Se outra máquina já estiver fazendo o dump, aquele banco é ignorado com um aviso; se todos forem ignorados, a execução termina como "já em andamento" (código de saída 4) e não gera dump duplicado.
- **Como a trava é segurada:** uma sessão do `psql`/`mysql` fica aberta durante o dump (com `application_name = synkro-lock@<máquina>` no PostgreSQL, visível em `pg_stat_activity`). Ao terminar o dump, o Synkro encerra essa sessão e o servidor solta a trava sozinho — também se o worker morrer, porque o cliente roda no mesmo Job Object. Não há trava "presa" para limpar à mão.
- `remote.server_lock` vale `true` por padrão quando o host não é local; pode ser ligado também para bancos locais compartilhados.
- O `.dump.json` e o histórico mostram qual máquina gerou cada dump.

### 4A.4. Teste de restauração em servidor remoto

O `restore_test` cria um banco temporário. Em servidores de produção isso exige permissão de criar banco (`CREATEDB` / `CREATE`), que normalmente não se dá ao usuário de backup. Opções, em ordem de preferência:

1. `restore_test.connection` apontando para um **servidor de teste** separado (pode ser um PostgreSQL/MySQL instalado na própria estação);
2. um usuário específico de teste no servidor de produção, com permissão só para criar e apagar bancos com prefixo `synkro_verify_`.

---

## 5. Onde os dumps ficam nos cofres

Dumps são **imutáveis**: cada execução gera arquivos novos, nada é sobrescrito. Por isso tarefas `database` não usam `current\`/`versions\`; usam `dumps\`.

```
X:\SynkroVault\
└── jobs\
    └── <job-id>\
        ├── job.yml                         configuração da tarefa (sem segredos)
        ├── dumps.jsonl                     catálogo de todos os dumps desta tarefa
        ├── dumps.jsonl.sha256
        ├── dumps\
        │   ├── pdv_vendas\
        │   │   ├── 2026-11-17T220000Z_r-8f3a.dump
        │   │   ├── 2026-11-17T220000Z_r-8f3a.dump.json
        │   │   ├── 2026-11-18T220000Z_r-91c2.dump
        │   │   └── 2026-11-18T220000Z_r-91c2.dump.json
        │   └── _globals\
        │       └── 2026-11-18T220000Z_r-91c2.sql.zst
        ├── runs\
        │   └── <run-id>.summary.json
        └── .tmp\
```

Nome do arquivo: `<início do run em UTC>_<run-id>.<extensão>` — ordenável por data e ligado ao histórico.

### 5.1. Arquivo de metadados (`.dump.json`)

Gravado ao lado de cada dump. Permite entender e restaurar o arquivo **sem o Synkro e sem o banco local**:

```json
{
  "format": "synkro-dump",
  "format_version": 1,
  "job_id": "pdv-postgres",
  "run_id": "r-91c2",
  "engine": "postgresql",
  "database": "pdv_vendas",
  "server_version": "16.4",
  "tool": "C:/Program Files/PostgreSQL/16/bin/pg_dump.exe",
  "tool_version": "16.4",
  "dump_format": "custom",
  "compression": "pg-internal",
  "started_at": "2026-11-18T22:00:03Z",
  "finished_at": "2026-11-18T22:01:41Z",
  "size_bytes": 184223744,
  "sha256": "c1e9...",
  "validation": { "method": "pg_restore --list", "result": "ok", "table_count": 87 },
  "snapshot": { "tables": 87, "estimated_rows": 4120331 },
  "command": "pg_dump --host=localhost --port=5432 --username=synkro_backup --format=custom --compress=6 pdv_vendas",
  "restore_hint": "pg_restore --dbname=<novo_banco> --no-owner --jobs=4 \"2026-11-18T220000Z_r-91c2.dump\""
}
```

O campo `command` é registrado **sem** senhas e sem caminhos de arquivos de credenciais.

---

## 6. Credenciais

- **Senhas nunca ficam no YAML em texto puro.** Um YAML com `password: "abc"` é rejeitado pelo validador.
- O YAML referencia um **segredo**: `password: { secret: pg-pdv }` ou uma variável de ambiente: `password: { env: PDV_PG_PASS }`.
- Segredos são cadastrados pelo desktop ou por `synkro-worker secret set <nome>` (executado como administrador), e guardados em `C:\ProgramData\Synkro\secrets.dat`, cifrados com **DPAPI em escopo de máquina** (`CryptProtectData` + `CRYPTPROTECT_LOCAL_MACHINE`). Esse arquivo tem ACL de leitura apenas para SYSTEM e Administradores.
- Durante o dump, a credencial é gravada num arquivo temporário (`PGPASSFILE` ou `--defaults-extra-file`) em `C:\ProgramData\Synkro\run\<run-id>\`, com ACL **somente SYSTEM**, e apagada no fim do run (também em falha; na próxima inicialização, sobras são removidas).
- Os processos filhos rodam dentro de um **Job Object** do Windows: se o worker morrer ou estourar o tempo limite, a ferramenta de dump é encerrada junto.

---

## 7. Guardas específicas de banco

| Guarda | Regra padrão | Ação |
|---|---|---|
| Banco inacessível | Conexão falha após 3 tentativas (intervalo de 30 s) | Falha + alerta |
| Banco vazio | 0 tabelas quando o último dump tinha > 0 | **Bloqueia** (dump não substitui nada, mas o alerta é crítico) |
| Tabelas desaparecidas | Tabelas presentes no último dump que não existem mais | Aviso (ou bloqueio, se `on_missing_tables: block`) |
| Queda brusca de tamanho | Dump < 50% do tamanho do anterior | Aviso crítico; o dump é guardado, mas **não conta** para a retenção apagar dumps antigos |
| Tempo limite | `timeout` (padrão 2 h) | Processo encerrado, Falha |

Um dump que falhou ou foi bloqueado nunca é gravado nos cofres, e **a retenção nunca apaga dumps antigos com base num dump inválido**.

---

## 8. Retenção de dumps

Estratégia avô-pai-filho (GFS), por tarefa:

| Parâmetro | Padrão | Significado |
|---|---|---|
| `keep_last` | 7 | Os 7 dumps válidos mais recentes |
| `keep_daily` | 14 | O último dump de cada um dos últimos 14 dias |
| `keep_weekly` | 8 | O último dump de cada uma das últimas 8 semanas |
| `keep_monthly` | 12 | O último dump de cada um dos últimos 12 meses |

Um dump é mantido se **qualquer** regra o mantiver. Nunca é apagado o último dump válido de cada banco. A retenção roda por cofre e só após um run com status Sucesso.

---

## 9. Restauração

O Synkro **nunca restaura sobre o banco em uso por padrão.** A restauração sempre vai para um banco **novo** (ou arquivo novo, no SQLite), e substituir o banco de produção é uma operação separada, com confirmação dupla.

```
synkro-worker restore-db --job pdv-postgres --database pdv_vendas --dump latest --target-database pdv_vendas_restaurado
```

| Motor | O que o Synkro executa |
|---|---|
| PostgreSQL | confere SHA-256 → `createdb <alvo>` → `pg_restore --dbname=<alvo> --no-owner --jobs=<n> <arquivo>` → compara contagem de tabelas com os metadados |
| MySQL/MariaDB | confere SHA-256 → `CREATE DATABASE <alvo>` → descomprime em fluxo para `mysql <alvo>` → compara contagem de tabelas |
| SQLite | confere SHA-256 → descomprime para `<alvo>.db` → `PRAGMA integrity_check` |
| Personalizado | executa `restore.command` do YAML, se definido |

Sem o Synkro, o `restore_hint` do `.dump.json` traz o comando manual. Arquivos `.zst` são abertos com `zstd -d` (ferramenta gratuita).

### 9.1. Teste de restauração automático (recomendado)

Um dump só é realmente bom se restaurar. Com `restore_test` habilitado no YAML, o Synkro, na frequência configurada (padrão semanal):

1. pega o dump válido mais recente de cada banco no primeiro cofre conectado da tarefa (ou no indicado com `--vault`), conferindo o SHA-256;
2. restaura num banco temporário `synkro_verify_<id>` (no servidor indicado em `restore_test.connection`, que pode ser o mesmo ou outro) — no SQLite, num arquivo temporário com `integrity_check`;
3. confere se todas as tabelas registradas no dump existem no banco restaurado;
4. executa as consultas de verificação definidas pelo usuário (`checks`): cada uma precisa devolver verdadeiro (`t`, `true` ou `1`) na primeira coluna;
5. apaga o banco temporário (`DROP DATABASE IF EXISTS`), também quando a restauração falha no meio;
6. registra o resultado no `.dump.json` (`restore_test` e `restore_tested_at`, visíveis em `synkro-worker dumps`). Falha → alerta crítico `restore_test_failed`, resolvido no próximo teste bom.

O `config apply` registra a tarefa `\Synkro\restoretest-<tarefa>` no Agendador (padrão: domingo às 04:00, ou `restore_test.schedule`). Para rodar na hora:

```
synkro-worker restore-test --job pdv-postgres [--vault E:\] [--database pdv_vendas]
```

```yaml
restore_test:
  enabled: true
  schedule: { weekly: { days: [sun], at: "04:00" } }
  connection: same            # ou { host, port, user, password: { secret: ... } } de um servidor de teste
  checks:
    - name: "há vendas no último mês"
      sql: "SELECT count(*) > 0 FROM vendas WHERE data >= now() - interval '31 days'"
```

> A contagem de linhas por tabela (`exact_count_max_rows`) ainda não é comparada: o dump não registra o retrato de linhas no momento da exportação. A conferência é por tabelas e pelas verificações SQL.

---

## 10. Agendamento e combinação com backup de arquivos

- Tarefas `database` usam o mesmo agendador (Task Scheduler) e os mesmos cofres das tarefas `files`.
- Uma tarefa `files` pode declarar `depends_on: [pdv-postgres]`: ela só começa depois que a tarefa de banco terminar (sucesso ou não, conforme `depends_mode`). Exemplo comum: às 22:00 o dump do banco, em seguida os XMLs fiscais e, por fim, o desligamento.
- O desligamento automático considera **todas** as tarefas do grupo.

---

## 11. Testes obrigatórios

| Teste | Descrição |
|---|---|
| Ida e volta por motor | Popular banco de teste → dump → restaurar em banco novo → comparar esquema e dados (checksum por tabela) |
| Escrita concorrente | Inserções contínuas durante o dump; o dump restaurado precisa ser consistente (sem linhas órfãs de FK) |
| Ferramenta ausente / versão incompatível | Mensagem clara, Falha antes de qualquer gravação |
| Morte do processo | Matar `pg_dump`/`mysqldump` no meio; nada parcial nos cofres, temporários limpos |
| Credencial errada | Falha sem expor a senha em logs |
| Vazamento de segredo | Varredura dos logs, eventos, `.dump.json` e lista de processos em busca da senha usada no teste |
| SQLite em WAL | Backup com gravações ativas; `integrity_check` ok |

Os testes de integração usam PostgreSQL e MySQL em contêineres no CI e instalações reais num ambiente Windows de homologação.
