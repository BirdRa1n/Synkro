# Synkro — Roteiro do piloto em PDVs

Este é o critério de saída das Fases 2 a 4 ([architecture.md §15](architecture.md#15-roadmap)): o Synkro rodando sozinho em PDVs reais, sem perder dados e sem que nenhuma falha passe despercebida.

O piloto usa a versão **0.9.0**. O pacote não é assinado, então o Windows mostra o aviso do SmartScreen na primeira execução ("Mais informações → Executar assim mesmo").

---

## 1. Objetivo e critérios de sucesso

| Critério | Como medir | Meta |
|---|---|---|
| Nenhuma perda | Restaurações conferidas (seção 6) | 100% dos arquivos e dumps restaurados com SHA-256 conferido |
| Nenhuma falha silenciosa | Toda falha provocada (seção 5) gerou alerta na bandeja **e** notificação externa | 100% |
| Backup sem intervenção | Dias com backup bem-sucedido sem ninguém mexer no Synkro | ≥ 95% dos dias em que o PDV ficou ligado no horário |
| Sinal de vida | Relatório diário recebido | Todos os dias em que o PDV foi ligado |
| Instalação reprodutível | Tempo da instalação até o `doctor` dizer PRONTO | ≤ 1 h por PDV, seguindo só este roteiro |
| Operação compreensível | O responsável da loja entende os alertas e troca o disco de rodízio sozinho | Sim, sem ligação para o suporte |

**Duração:** 4 semanas. **Tamanho:** 2 ou 3 PDVs, de preferência com motores de banco diferentes e com um cofre em rede em pelo menos um deles.

**Pare o piloto** (seção 8) se houver:
- qualquer perda de dado;
- um backup que deu "sucesso" mas não restaura;
- instabilidade no PDV atribuível ao Synkro (lentidão no horário de venda, travamento).

---

## 2. Escolha dos PDVs e pré-requisitos

Para cada PDV, levante antes:

| Item | Exemplo |
|---|---|
| Windows e acesso de administrador | Windows 10 Pro 22H2, conta local "suporte" |
| Pastas a proteger | `C:\PDV\XML`, `C:\PDV\Config` |
| Banco de dados, motor, versão, onde roda | PostgreSQL 15 local; ou MySQL num servidor da loja |
| Usuário de backup do banco | Só leitura + o necessário para o dump ([database-backup.md §5](database-backup.md)) |
| Horário sem movimento | 23:00 às 07:00 |
| Cofres | HD externo USB **de rodízio** (2 discos alternados) + HD interno secundário **ou** NAS |
| Canal de aviso | E-mail do suporte e/ou Telegram do dono |
| Responsável na loja | Nome e telefone de quem troca o disco de rodízio |

Mínimo por PDV: **dois cofres em discos físicos diferentes** (o `doctor` avisa quando uma tarefa tem um só) e **um canal de notificação com relatório diário**.

Use **cofre cifrado** nos discos que saem da loja e no NAS. Anote a chave de recuperação na ficha do PDV, guardada **fora** do PDV.

---

## 3. Antes de ir à loja

1. Gere o pacote: `powershell -ExecutionPolicy Bypass -File installer\package.ps1`. Confira o `SHA256SUMS` do zip.
2. Leve: o zip, os discos de rodízio **novos** (vazios), esta folha e a ficha do PDV.
3. Se houver NAS, crie a pasta do PDV e um usuário só do Synkro com permissão de gravação nela ([configuration.md §5.2](configuration.md#52-cofres-em-rede-nas-ou-servidor)).
4. Prepare o canal de aviso: senha de app do e-mail ou o token do bot e o `chat_id` do Telegram.

---

## 4. Instalação no PDV (≈ 1 h)

Tudo como **administrador**.

1. **Instale:** extraia o zip e rode `powershell -ExecutionPolicy Bypass -File install.ps1` ([operations.md §1.2](operations.md#12-instalar-ou-atualizar)).
2. **Cadastre os cofres:** Synkro → **Cofres → Adicionar cofre**. Disco externo: marque **rodízio** e **Cifrar**; anote a chave de recuperação. NAS: **Pasta na rede**.
3. **Cadastre os segredos:** senha do banco e do SMTP, token do Telegram (Configurações → Segredos ou `synkro-worker secret set <nome>`).
4. **Crie as tarefas:** **Tarefas → Nova tarefa**, uma para os arquivos e uma para o banco. No banco, ligue o **teste de restauração** semanal (`restore_test`).
5. **Configure as notificações** no `synkro.yml`, com `daily_report: "08:00"` ([configuration.md §5.1](configuration.md#51-notificações-externas-notifications)). Teste: `synkro-worker notify test`.
6. **Aplique:** Configurações → **Aplicar e agendar**.
7. **Rode a checagem:** `synkro-worker doctor` até aparecer **PRONTO** (ou PRONTO COM RESSALVAS, com cada aviso entendido e anotado na ficha).
8. **Primeiro backup, na sua frente:** Tarefas → Executar agora, em cada tarefa. Ele é completo e pode demorar.
9. **Restauração de amostra:** Restaurar → uma pasta pequena para `D:\Teste-Restauracao`. Abra 2 ou 3 arquivos. Para o banco, `restore-db` para um banco **novo** e confira uma consulta.
10. **Registre:** `synkro-worker diag --to piloto-<loja>-dia0.txt` (ou Configurações → Gerar diagnóstico). Guarde com a ficha.

---

## 5. Testes de falha provocada (semana 1)

Faça cada teste **fora do horário de venda**. Anote, em cada um, se houve alerta na bandeja, notificação externa e recuperação sozinha.

| # | Provocação | Esperado |
|---|---|---|
| F1 | Desconectar o HD externo **durante** um backup | Execução **degradada** com o outro cofre ok; alerta; no próximo backup com o disco conectado, ele é atualizado e a execução interrompida é recuperada |
| F2 | Desligar o PDV da tomada **durante** um backup | Ao ligar: o backup perdido roda sozinho ("executar ao ligar"); nenhum arquivo pela metade no cofre (`synkro-worker verify --vault <cofre> --job <id>`) |
| F3 | Desligar o NAS (ou o cabo de rede) | O backup grava nos outros cofres e termina em poucos segundos no NAS, com "não responde"; alerta crítico depois de `max_absence` |
| F4 | Trocar a senha do usuário do banco sem atualizar o Synkro | Falha da tarefa de banco com mensagem clara; alerta e notificação; volta ao normal com `secret set` |
| F5 | Encher o cofre: ajuste `free_space_alert` para acima do uso atual | Alerta de pouco espaço; volte o valor |
| F6 | Apagar uma pasta de teste com muitos arquivos **criada para isso** dentro da origem | Backup **bloqueado** pela guarda de exclusão em massa; nada some do cofre; liberar com `--confirm-mass-change` |
| F7 | Ficar 2 dias sem conectar o disco de rodízio de volta | Alerta de cofre ausente conforme `max_absence` |
| F8 | Restaurar num **outro computador** a partir do disco de rodízio cifrado | Pede a chave de recuperação; restaura com SHA-256 conferido |
| F9 | Canal de notificação inválido (token errado de propósito) | Alerta `notify_failed` na bandeja; volta ao normal ao corrigir |

Nunca provoque falhas no banco de produção nem apague dados reais. F6 é feito numa pasta de teste criada só para isso.

---

## 6. Acompanhamento

**Todo dia** (o suporte, pelo relatório diário):
- O relatório chegou? Se não chegou, ligue para a loja: o PDV ficou desligado ou o Synkro parou?
- Algum alerta crítico aberto?

**Toda semana:**
- `diag` de cada PDV (Configurações → Gerar diagnóstico), arquivado com a data.
- Uma restauração de amostra de um dia aleatório, em pasta nova.
- O teste de restauração automático do banco rodou e passou? (`synkro-worker dumps`)
- Troca do disco de rodízio feita pela loja.

**Registro de ocorrências** (uma linha por evento):

| Data | PDV | O que aconteceu | Alerta? | Notificação? | Precisou de intervenção? | Causa | Ação |
|---|---|---|---|---|---|---|---|

---

## 7. Avaliação no fim das 4 semanas

Com os `diag` semanais e o registro, preencha:

- Dias com backup bem-sucedido / dias com o PDV ligado no horário.
- Duração típica de cada tarefa (o maior valor também) e o tamanho de cada cofre.
- Falhas reais (não provocadas), causa e se foram notificadas.
- Intervenções feitas, quem fez e quanto tempo levou.
- Resultado de cada teste da seção 5.
- O que a loja achou: entende os alertas? A troca do disco é viável?

**Decisão:**
- **Aprovado:** todos os critérios da seção 1 atingidos.
- **Aprovado com correções:** só critérios operacionais falharam (documentação, mensagens, tempo de instalação). Corrija e siga.
- **Reprovado:** houve perda, backup que não restaura ou falha silenciosa. Corrija, adicione um teste automatizado que reproduza o caso e repita o piloto.

---

## 8. Interromper ou desinstalar

1. Gere um `diag` antes de tudo.
2. Desinstale: `powershell -ExecutionPolicy Bypass -File uninstall.ps1`. Isso remove o programa e as tarefas do Agendador, e mantém `C:\ProgramData\Synkro` (configuração e estado), a menos que se use `-RemoveData`.
3. Os cofres **nunca** são apagados pela desinstalação. Os backups continuam restauráveis com o Synkro em qualquer outro computador (cofre cifrado: com a chave de recuperação).
