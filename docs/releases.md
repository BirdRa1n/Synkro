# Synkro — Versões, releases e atualização automática

## 1. Numeração

A versão segue o [semver](https://semver.org/lang/pt-BR/): `MAJOR.MINOR.PATCH`, com sufixo opcional de pré-lançamento.

| Versão | Tipo | Quem recebe pelo atualizador |
|---|---|---|
| `0.9.0-beta.1`, `0.9.0-rc.1` | Pré-lançamento (beta) | Só quem ligou **Receber versões beta**. Quem já roda uma beta começa com essa opção ligada. |
| `0.9.0` | Estável | Todos |

A parte numérica fica no `CMakeLists.txt` (`project(Synkro VERSION 0.9.0)`). O sufixo vem da tag, na hora de publicar: o mesmo código gera `0.9.0-beta.1`, `0.9.0-beta.2` e por fim `0.9.0`. Precedência: `0.9.0-beta.1 < 0.9.0-beta.2 < 0.9.0-rc.1 < 0.9.0 < 0.9.1-beta.1`.

## 2. Publicar uma versão (aba Releases)

O workflow [`.github/workflows/release.yml`](../.github/workflows/release.yml) compila no Windows, roda todos os testes, gera o pacote e o instalador e publica a release. A cada push, o [`ci.yml`](../.github/workflows/ci.yml) compila e testa sem publicar nada.

Os dois usam o **Qt 6.8 LTS** (MinGW 13.1): o instalador de Qt usado pelo GitHub Actions (aqtinstall) ainda não instala o 6.12 da máquina de desenvolvimento. O código só usa APIs presentes desde o Qt 6.4. Há dois jeitos de disparar:

- **Pelo GitHub:** **Actions → Release (Windows) → Run workflow**, informando a versão (ex.: `0.9.0-beta.2`). A tag `v0.9.0-beta.2` é criada no commit escolhido.
- **Pela linha de comando:**

  ```powershell
  git tag v0.9.0-beta.2
  git push origin v0.9.0-beta.2
  ```

O workflow recusa:
- versão inválida;
- parte numérica diferente da do `CMakeLists.txt`;
- tag que já existe;
- qualquer teste falhando (nesse caso nada é publicado).

**Arquivos de cada release:**

| Arquivo | Para quê |
|---|---|
| `Synkro-Setup-<versão>.exe` | Instalador (Inno Setup). Instala ou atualiza: extrai o pacote e roda o `install.ps1`. |
| `Synkro-Setup-<versão>.exe.sha256` | Soma conferida pelo atualizador do app antes de instalar |
| `Synkro-<versão>-win64.zip` (+ `.sha256`) | Mesmo conteúdo, para instalação manual (`install.ps1`) |

Versões com sufixo saem marcadas como **pré-lançamento**; as estáveis, como **Latest**. As notas trazem as instruções de instalação e a lista de mudanças gerada pelo GitHub.

Para gerar o mesmo localmente, com o [Inno Setup 6](https://jrsoftware.org/isinfo.php) instalado:

```powershell
powershell -ExecutionPolicy Bypass -File installer\package.ps1 -VersionSuffix beta.2 -Installer
```

## 3. Atualização automática no app

O Synkro Desktop se atualiza no estilo do electron-updater.

1. **Procura:** 20 s depois de abrir e a cada 6 h, o app consulta as releases do repositório (`SYNKRO_UPDATE_REPO` no CMake; padrão `BirdRa1n/Synkro`). Considera só releases que tenham o instalador e o `.sha256`.
2. **Baixa:** em segundo plano, para `%TEMP%\Synkro-update`, conferindo o SHA-256 publicado. Um download que não confere é apagado.
3. **Avisa:** a bandeja mostra "A versão X foi baixada. Clique para instalar." Em **Configurações → Atualizações**, o botão **Instalar atualização** aparece.
4. **Instala:** depois da confirmação, o app executa o instalador em modo silencioso (pede administrador) e se fecha. O instalador espera um backup em andamento terminar, atualiza os programas, mantém a configuração e os cofres, reaplica as tarefas do Agendador e reabre o Synkro.

Em **Configurações → Atualizações**:
- versão instalada, canal e última verificação;
- **Procurar atualizações**;
- **Receber versões beta**;
- **Baixar atualizações automaticamente** (a instalação sempre pede confirmação);
- link para as novidades da versão.

**Segurança:**
- **Origem:** tudo é baixado por HTTPS do GitHub, sem aceitar redirecionamento para http, e o instalador só roda se o SHA-256 conferir com o publicado na mesma release.
- **Limite:** enquanto os executáveis não forem assinados, essa conferência detecta download corrompido, mas não substitui uma assinatura digital. Quem controla a conta do GitHub controla as atualizações.
- **Com certificado:** passe `-CertificateThumbprint` ao `package.ps1` (os executáveis e o instalador são assinados). O próximo passo é o atualizador exigir assinatura válida.
- **Desligar:** compile com `-DSYNKRO_UPDATE_REPO=` (vazio) para um build sem atualização automática.
