# Synkro

Backup incremental, versionado e redundante para dados críticos no Windows, com foco em sistemas PDV: arquivos fiscais, configurações e bancos de dados (PostgreSQL, MySQL/MariaDB, SQLite).

> **Estado:** em desenvolvimento — Fase 1 (motor e CLI). Veja o [roadmap](docs/architecture.md#15-roadmap).

## Princípios

- Nunca destruir o único exemplar de um dado: exclusões na origem não apagam o backup.
- Cada disco de backup (**cofre**) é autossuficiente e fica num **disco físico diferente** da origem.
- Nada é considerado copiado sem verificação (SHA-256, gravação forçada em disco, troca atômica).
- Falha silenciosa é o pior defeito: backup atrasado, disco ausente ou cheio geram alerta.

## Documentação

| Documento | Conteúdo |
|---|---|
| [docs/architecture.md](docs/architecture.md) | Arquitetura, cofres, fluxo de backup, recuperação de falhas, roadmap |
| [docs/database-backup.md](docs/database-backup.md) | Backup consistente de bancos de dados |
| [docs/configuration.md](docs/configuration.md) | Configuração em YAML |

## Estrutura

```
apps/desktop/   synkro-desktop (Qt Widgets)
apps/worker/    synkro-worker (CLI, executado pelo Agendador de Tarefas)
src/            libsynkro — motor (Qt Core + Win32, sem dependência de UI)
  core/         Result/Error, versão
  hash/         SHA-256 via Windows CNG
  platform/     utilitários Win32, volumes e discos físicos
  scan/         varredura da origem e detecção de mudanças
  transfer/     cópia segura com verificação e troca atômica
  vault/        identidade do cofre e checagem de discos candidatos
tests/          Qt Test (unit/ e integration/), integrados ao CTest
```

## Compilação

Requisitos: Windows 10/11, CMake ≥ 3.22, Ninja e Qt 6.5+ (kit MinGW 64-bit nesta fase; a migração para MSVC está prevista antes da Fase 2).

```powershell
$env:SYNKRO_QT_DIR = "C:/Qt/6.12.0/mingw_64"
$env:PATH = "C:\Qt\Tools\mingw1310_64\bin;C:\Qt\Tools\Ninja;C:\Qt\Tools\CMake_64\bin;$env:PATH"

cmake --preset mingw-debug
cmake --build --preset mingw-debug
ctest --preset mingw-debug
```

No Qt Creator basta abrir o `CMakeLists.txt` da raiz.

## Uso do worker (Fase 1)

```powershell
synkro-worker vault check E:\ --source C:\PDV     # o disco serve como cofre?
synkro-worker vault init  E:\ --label "HD Externo 1" --source C:\PDV
synkro-worker vault show  E:\
synkro-worker scan C:\PDV --ids
synkro-worker hash C:\PDV\dados\caixa.db --uncached
```

Códigos de saída: `0` sucesso · `2` falha · `3` bloqueado por verificação · `64` uso incorreto.

## Licença

Veja [LICENSE](LICENSE).
