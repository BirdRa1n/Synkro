# Synkro — Guia de operação

Para quem instala, configura e acompanha o Synkro num PDV ou servidor. Arquitetura em [architecture.md](architecture.md), bancos de dados em [database-backup.md](database-backup.md), YAML em [configuration.md](configuration.md).

---

## 1. Instalação

**Requisitos:** Windows 10 ou 11 (64 bits) ou Windows Server 2016+; um usuário administrador para instalar; pelo menos um disco de backup (cofre) **em outro disco físico** que não o do sistema/dados.

### 1.1. Gerar o pacote (equipe de desenvolvimento)

```powershell
powershell -ExecutionPolicy Bypass -File installer\package.ps1 [-CertificateThumbprint <thumbprint>]
```

Compila em Release, copia o Qt e o runtime com o `windeployqt`, acrescenta os scripts, a documentação e os exemplos, confere que o worker roda sem o Qt instalado e gera em `out\package\`:

- `Synkro-<versão>-win64\` — a pasta do pacote, com `SHA256SUMS.txt`;
- `Synkro-<versão>-win64.zip` e `.zip.sha256`.

Com `-CertificateThumbprint`, os executáveis são assinados com o `signtool` (Windows SDK) e carimbo de tempo. **Sem certificado, o pacote sai sem assinatura** e o SmartScreen avisa na primeira execução. O instalador MSI assinado depende do WiX Toolset e de um certificado de assinatura de código; enquanto isso, o pacote é a pasta/zip com `install.ps1`.

### 1.2. Instalar ou atualizar

Na máquina, extraia o zip, confira o hash (`Get-FileHash Synkro-...zip` contra o `.zip.sha256`) e, num PowerShell **como administrador**:

```powershell
cd C:\caminho\Synkro-0.1.0-win64
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

O que o `install.ps1` faz, em ordem:

1. confere o SHA-256 de **cada arquivo** do pacote (`SHA256SUMS.txt`) e avisa se os executáveis não têm assinatura válida;
2. **espera** um backup em andamento terminar (até 30 min, `-WaitForBackupMinutes`) — nunca interrompe o worker; fecha só a interface (desktop);
3. copia os programas para `C:\Program Files\Synkro` (espelho da pasta do pacote);
4. cria `C:\ProgramData\Synkro` com as permissões da seção 2 (e move o estado de versões antigas para `state\`);
5. registra a origem **Synkro** no Visualizador de Eventos (log *Aplicativo*);
6. liga o **início automático na bandeja** para todos os usuários (`HKLM\...\Run`, `synkro-desktop --tray`) — `-NoAutostart` desliga;
7. cria o atalho no menu Iniciar e põe a pasta no `PATH` (para usar `synkro-worker` no prompt);
8. registra em **Programas e Recursos** (desinstalação pelo Painel de Controle);
9. se já existir configuração, roda `synkro-worker config apply` (tarefas no Agendador) — `-NoApply` pula.

`-WhatIf` mostra tudo o que seria feito sem alterar nada. Para **atualizar**, rode o `install.ps1` da versão nova: configuração, segredos e histórico em `ProgramData` são mantidos.

### 1.3. Desinstalar

Pelo Painel de Controle (Programas e Recursos → Synkro) ou:

```powershell
powershell -ExecutionPolicy Bypass -File "C:\Program Files\Synkro\uninstall.ps1" [-RemoveData]
```

Remove programas, tarefas `\Synkro\` do Agendador, início automático, atalho e o registro. **Mantém** `C:\ProgramData\Synkro` (configuração, segredos, histórico) a menos que se use `-RemoveData`, que pede confirmação. **Os cofres e os backups dentro deles nunca são apagados.**

---

## 2. Permissões

| Pasta / arquivo | SYSTEM | Administradores | Usuários | Por quê |
|---|---|---|---|---|
| `C:\Program Files\Synkro` | total | total | leitura | Padrão do Windows |
| `C:\ProgramData\Synkro` | total | total | leitura | O desktop lê a configuração; mudanças passam pelo worker elevado (UAC) |
| `...\config\` | total | total | leitura | Idem |
| `...\state\` (histórico, alertas) | total | total | **modificar** | O desktop roda como usuário e atualiza alertas; o SQLite em WAL cria arquivos ao lado do banco. É cache: os cofres são a fonte da verdade |
| `...\secrets.dat` | total | total | — | Senhas cifradas com DPAPI de máquina |
| `...\staging\` | total | total | — | Dumps em preparo e credenciais temporárias (cada execução cria uma subpasta só do dono e do SYSTEM) |

As tarefas agendadas rodam como **SYSTEM**. O desktop roda como o usuário logado e pede elevação (UAC) só para o que exige administrador: aplicar a configuração, cadastrar segredos e preparar discos.

---

## 3. Primeira configuração

Pelo **Synkro Desktop** (menu Iniciar):

1. **Cofres → Adicionar disco:** escolha o disco de backup; o Synkro confere se é outro disco físico, o sistema de arquivos, o espaço, a gravação e a **saúde SMART**, e grava a identidade (`vault.json`). Repita para o segundo disco (duplicidade).
2. **Configurações → Segredos:** cadastre as senhas dos bancos (ficam cifradas; o YAML guarda só o nome).
3. **Tarefas → Nova tarefa:** o assistente cria tarefas de arquivos ou de banco (PostgreSQL, MySQL/MariaDB, Firebird, SQL Server, SQLite), com teste de conexão e um resumo em português do que será feito.
4. **Configurações → Aplicar e agendar (administrador):** registra as tarefas no Agendador do Windows.

Pela linha de comando (prompt como administrador): `vault init E:\ --label "HD Externo 1"`, `secret set pg-pdv`, editar os YAMLs em `C:\ProgramData\Synkro\config`, `config validate`, `config plan`, `config apply`.

**Vários PDVs com a mesma configuração:** configure um, exporte (**Configurações → Exportar…** ou `config export --to pacote.yml`) e importe nos demais depois de preparar os discos: **Configurações → Importar…** (escolha o pacote, ligue cada cofre a um disco, **Simular** e **Importar**) ou `config import --from pacote.yml --bind hd-externo-1=E: --bind hd-interno-2=F:` (detalhes em [configuration.md §12](configuration.md#12-exportar-e-importar)). O pacote não leva senhas nem a identidade dos discos.

---

## 4. Tarefas no Agendador do Windows

Todas ficam na pasta `\Synkro\` do Agendador e rodam como SYSTEM:

| Tarefa | Quando | O que faz |
|---|---|---|
| `job-<id>` | `schedule` da tarefa | `synkro-worker backup --job <id> --trigger schedule` |
| `restoretest-<id>` | `restore_test.schedule` (padrão domingo 04:00) | Teste de restauração do banco num temporário |
| `Maintenance` | a cada 15 min e 2 min após ligar | Aplica a configuração editada à mão, limpa preparo antigo, reavalia alertas |
| `Scrub` | `maintenance.scrub.schedule` | Relê uma amostra dos cofres sem cache e repara a partir de outro cofre |

Com `schedule_catch_up: true` (padrão), um horário perdido com o PC desligado roda assim que ele ligar. `synkro-worker schedule show --job <id>` mostra o XML registrado.

---

## 5. Acompanhamento diário

- **Bandeja:** o ícone muda de cor com alertas abertos e mostra uma notificação quando surge um alerta novo. Abre o painel com um clique.
- **Painel:** situação de cada tarefa (último sucesso, próximo horário), dos cofres (conectado, espaço, saúde do disco) e os alertas.
- **Visualizador de Eventos:** log *Aplicativo*, origem **Synkro** — alertas novos como Aviso/Erro e resolvidos como Informação. Útil para monitoramento remoto (encaminhamento de eventos, RMM).
- **Linha de comando:** `synkro-worker status` (código de saída 2 se houver alerta crítico).

### 5.1. Alertas

| Código | Gravidade | Significado | O que fazer |
|---|---|---|---|
| `config_invalid` | crítico | A configuração tem erros; vale a última válida | `synkro-worker config validate` mostra arquivo e linha |
| `no_success` | crítico | Tarefa sem sucesso há mais de `alerts.no_success_for` (padrão 26 h) | Ver o histórico; conferir se o PC fica ligado no horário |
| `never_run` | aviso | Tarefa ainda não executada | Rodar uma vez (Tarefas → Executar agora) |
| `last_run_failed` | crítico | Última execução falhou | Ver o motivo no histórico; `synkro-worker runs` |
| `last_run_blocked` | crítico | Uma guarda bloqueou (origem vazia, exclusão/modificação em massa, banco vazio) | Ver seção 7.1 |
| `last_run_degraded` | aviso | Algum cofre não recebeu o backup (ex.: desconectado) | Conectar o cofre |
| `vault_absent` | aviso/crítico | Cofre não visto há mais de `max_absence` (crítico se não for de rodízio) | Conectar o disco |
| `low_space` | aviso/crítico | Espaço livre abaixo de `free_space_alert` | Ajustar a retenção ou trocar por um disco maior |
| `disk_health` | aviso/crítico | Disco de cofre com sinais de falha (SMART) | **Trocar o disco** (seção 8) |
| `source_disk_health` | aviso/crítico | O disco da **origem** dá sinais de falha | Urgente: o PDV corre risco; conferir o backup e trocar o disco |
| `scrub_problems` | crítico | A verificação periódica achou arquivos corrompidos/ausentes que não conseguiu reparar | Rodar `verify` no cofre; considerar trocar o disco |
| `restore_test_failed` | crítico | O teste de restauração de um banco falhou | `synkro-worker restore-test --job <id>` mostra o detalhe |
| `notify_failed` | aviso | Um canal de notificação não consegue entregar (3 tentativas) | `synkro-worker notify list` mostra o erro; ver seção 5.2 |

Alertas se resolvem sozinhos quando a condição some.

### 5.2. Avisos por e-mail, webhook e Telegram

Configure os canais em `notifications` no `synkro.yml` ([configuration.md §5.1](configuration.md#51-notificações-externas-notifications)).

1. Cadastre as senhas e os tokens como administrador: `synkro-worker secret set smtp-suporte`.
2. Teste cada canal: `synkro-worker notify test` (ou `--channel ID` para um só). A mensagem de teste é enviada na hora, fora da fila.
3. Pronto: os alertas novos saem ao fim de cada backup e na manutenção (a cada 15 min).

Para ver o que foi enviado, o que está pendente e o último erro de cada envio: `synkro-worker notify list`. Para reenviar já os pendentes, sem esperar o intervalo de nova tentativa: `synkro-worker notify`.

| Erro | Causa provável |
|---|---|
| `servidor não encontrado (DNS)` | Endereço errado ou PDV sem internet |
| `o servidor SMTP não oferece STARTTLS` | Servidor que usa TLS direto: `security: tls`, porta 465 |
| `o servidor SMTP recusou autenticação: 535 ...` | Usuário ou senha errados. No Gmail e no Microsoft 365, use uma *senha de app*. |
| `falha no TLS (certificado inválido ou não confiável)` | Certificado próprio de servidor interno: instale-o no Windows ou use `allow_insecure: true` |
| `o servidor respondeu HTTP 401/403 (token recusado)` | Token do webhook ou do bot errado ou revogado |
| `o servidor respondeu HTTP 404` | O webhook foi apagado no Slack, Discord ou Teams. Para o Telegram, confira o `chat_id` e se o bot está no grupo. |
| `sem acesso aos segredos` | O comando foi rodado por um usuário comum. O worker agendado (SYSTEM) entrega na próxima passada. |

O **relatório diário** (`daily_report: "08:00"`) é o sinal de vida do PDV: se ele não chegar num dia, o computador ficou desligado ou o Synkro parou de rodar.

---

## 6. Restauração

O Synkro **nunca sobrescreve**: a restauração vai sempre para uma pasta, banco ou arquivo **novo**, e o SHA-256 de cada arquivo é conferido.

- **Arquivos:** desktop → **Restaurar**, escolha a tarefa, a execução (ponto no tempo) e a pasta de destino. Linha de comando: `synkro-worker restore --vault E:\ --job pdv-arquivos --to D:\Restaurado [--run r-...] [--path pasta/arquivo]`.
- **Bancos:** desktop → **Restaurar** (aba de dumps) ou `synkro-worker restore-db --vault E:\ --job pdv-postgres --database pdv_vendas --target-database pdv_vendas_restaurado`. Firebird e SQLite usam `--to <arquivo novo>`.
- **Sem o Synkro:** os arquivos em `current\` do cofre são cópias normais; cada dump tem ao lado um `.dump.json` com o comando de restauração manual (`restore_hint`). Num **cofre cifrado** isso não vale: o conteúdo só é lido pelo Synkro, com a chave (seção 7.5).

Trocar o banco de produção pelo restaurado é uma decisão do responsável pelo sistema e fica fora do Synkro.

---

## 7. Situações comuns

### 7.1. Backup bloqueado por uma guarda

As guardas existem para não propagar um desastre para os cofres (pasta apagada, ransomware, banco vazio). Antes de liberar:

1. confira a origem: os arquivos sumiram ou foram alterados de verdade? há sinais de ransomware (extensões estranhas, notas de resgate)?
2. se a mudança é legítima (ex.: limpeza de XMLs antigos), rode uma vez com confirmação: `synkro-worker backup --job <id> --confirm-mass-change`.

Nada foi apagado dos cofres: a versão anterior continua lá.

### 7.2. Disco de cofre trocado de letra

Não há o que fazer: o cofre é encontrado pelo `vault_id`, não pela letra.

### 7.3. Trava no servidor ("dump em andamento em outra máquina")

Duas máquinas estão configuradas para o mesmo banco num servidor compartilhado. A segunda execução é ignorada (código de saída 4). Deixe a tarefa em uma única máquina.

### 7.4. Cofre em rede inacessível

O alerta e o `synkro-worker status` mostram o motivo.

| Motivo | O que fazer |
|---|---|
| `o servidor ... não responde (porta 445 ...)` | NAS desligado, fora da rede ou firewall bloqueando o compartilhamento de arquivos |
| `servidor não encontrado na rede` / `(DNS)` | Nome errado no `path`; teste com o IP |
| `usuário ou senha recusados pelo servidor` | Senha trocada no NAS: `synkro-worker secret set <nome>` com a nova |
| `o compartilhamento ... não existe no servidor` | O compartilhamento foi renomeado ou removido |
| `acesso negado a ...` | O usuário perdeu a permissão de gravação na pasta |
| `o compartilhamento respondeu, mas não há cofre do Synkro em ...` | A pasta foi apagada ou movida no NAS. Restaure-a ou crie um cofre novo (`vault init`) e troque o `vault_id`. |

**Restaurar a partir do NAS:**
- **Desktop:** em **Restaurar**, os cofres em rede declarados e acessíveis aparecem na lista. Em **Cofre em rede…**, você informa a pasta, o usuário e a senha; isso funciona até num computador novo, sem configuração, porque o cofre é autossuficiente. Nada é gravado no NAS.
- **Linha de comando:** `synkro-worker restore --vault \\nas01\backup\pdv01 --job pdv-arquivos --to D:\Restaurado`. Se a sessão não tiver acesso ao NAS, rode `net use \\nas01\backup /user:nas01\synkro` antes.

### 7.5. Cofre cifrado e chave de recuperação

**Criar:**
- **Desktop:** Cofres → Adicionar cofre → marque **Cifrar o cofre (AES-256)**.
- **Linha de comando:** `synkro-worker vault init F:\ --label "HD Rodízio" --encrypt`.

Só um cofre novo, sem backups, pode ser cifrado. A **chave de recuperação** aparece uma única vez.
- Imprima ou anote e guarde-a **fora do PDV**: no cofre da loja, com o responsável, num gerenciador de senhas.
- Não a guarde num arquivo no próprio PDV, nem no próprio disco de backup.
- Vale um registro por cofre: cada cofre tem a sua chave.

| Situação | O que fazer |
|---|---|
| Uso normal | Nada: a chave fica guardada nesta máquina e o worker cifra sozinho |
| PDV reinstalado ou trocado, mesmos cofres | `synkro-worker vault unlock <disco>` como administrador (pede a chave de recuperação) |
| Restaurar num computador novo | Desktop: Restaurar, escolha o cofre e digite a chave quando pedida. Linha de comando: `restore` / `restore-db` também pedem. |
| Backup falha com "é cifrado e a chave não está disponível nesta máquina" | `vault unlock` no disco indicado |
| "este cofre era cifrado, mas o key.json sumiu" | Alguém apagou as duas cópias (`SynkroVault\key.json` e `key.backup.json`); se só uma some, o Synkro a refaz sozinho. O Synkro não grava em claro. Restaure os arquivos de outra cópia ou prepare um cofre novo. |
| Chave de recuperação perdida, PDV funcionando | Os backups continuam funcionando. Prepare um cofre **novo** cifrado (com chave nova, anotada desta vez) e aposente o antigo quando o novo tiver o histórico necessário. |
| Chave de recuperação perdida **e** PDV perdido | Os dados desse cofre não podem ser recuperados por ninguém |

O que a cifragem protege:
- **Protegido:** o conteúdo dos arquivos e dos dumps, se o disco ou o NAS cair em mãos erradas.
- **Não protegido:**
  - nomes de pastas e arquivos, tamanhos e datas continuam visíveis;
  - o `job.yml` e os `.dump.json` do cofre (nomes de servidores, bancos e tabelas) também;
  - quem levar o PDV junto com o disco consegue ler.

O botão **Copiar** da chave de recuperação marca o conteúdo para ficar fora do histórico da área de transferência (Win+V) e da sincronização com a nuvem.

Detalhes em [architecture.md §3.5](architecture.md#35-cofre-cifrado-fase-5).

### 7.6. Códigos de saída do worker

`0` sucesso · `1` degradado · `2` falha · `3` bloqueado · `4` já em execução / ignorado · `64` uso incorreto.

---

## 8. Trocar um disco de cofre

1. Conecte o disco novo e prepare-o (**Cofres → Adicionar disco** ou `vault init`).
2. No `synkro.yml`, troque o `vault_id` do cofre pelo do disco novo (ou remova o antigo e acrescente o novo pelo desktop).
3. Aplique e rode as tarefas: o primeiro backup no disco novo é completo.
4. Guarde o disco antigo até o novo acumular o histórico necessário: os backups dele continuam legíveis e restauráveis (`restore --vault <disco antigo>`).

---

## 9. Teste de estresse

Para medir o desempenho numa máquina (ex.: antes de levar a um PDV com muitos XMLs):

```powershell
$env:SYNKRO_STRESS = '1'
$env:SYNKRO_STRESS_FILES = '100000'
$env:SYNKRO_STRESS_DIR = 'E:\'   # opcional: disco onde ficam origem, cofre e restauração
ctest --preset mingw-release -R tst_stress --output-on-failure -V
```

Pelo CTest o limite por função do QTest já sobe para 2 h. Rodando o `tst_stress.exe` direto, defina também `$env:QTEST_FUNCTION_TIMEOUT = '7200000'` (o padrão do QTest é 5 min).

Mede backup completo, incremental (1% alterado, 0,5% apagado, 1% novo), execução sem mudanças, verificação de 5% e restauração completa conferida por amostra. Sem `SYNKRO_STRESS=1` o teste é pulado.

Cada arquivo é gravado com flush e troca atômica (seguro contra queda de energia). Por isso o **primeiro** backup de muitos arquivos pequenos é limitado pelas gravações síncronas do disco. Os incrementais só copiam o que mudou.

**Referência** (2026-11-19, build Release, HD SATA de 7200 rpm, origem e cofre no mesmo disco — o pior caso para gravações síncronas; 100 mil XMLs de 0,1 a 1 KB):

| Etapa | Tempo | Ritmo |
|---|---|---|
| Backup completo (100 000 arquivos) | 8 min 3 s | 207 arq/s |
| Incremental (1 000 novos, 1 000 alterados, 500 apagados) | 18 s | — |
| Sem mudanças (100 500 arquivos conferidos) | 5,8 s | 17 191 arq/s |
| Verificação de 5% sem cache (4 961 arquivos) | 2,6 s | 1 895 arq/s |
| Restauração completa (100 500 arquivos) | 11 min 22 s | 147 arq/s |

Com o cofre em outro disco físico (a configuração exigida), leitura e gravação não competem e os tempos tendem a cair; SSDs reduzem bastante o custo do flush por arquivo.
