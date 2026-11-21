#include "Dialogs.h"

#include "AppContext.h"
#include "Format.h"
#include "Icons.h"
#include "Theme.h"
#include "Ui.h"

#include "config/ConfigLoader.h"
#include "config/ConfigPackage.h"
#include "config/ConfigWriter.h"
#include "config/PlanDescriber.h"
#include "config/VaultRemoval.h"
#include "engine/VaultOps.h"
#include "core/Paths.h"
#include "core/RunId.h"
#include "crypto/VaultKey.h"
#include "database/Drivers.h"
#include "database/Tools.h"
#include "platform/windows/NetworkShare.h"
#include "platform/windows/TaskScheduler.h"
#include "platform/windows/Win32.h"
#include "secrets/SecretStore.h"
#include "vault/Vault.h"
#include "vault/VaultCandidateCheck.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFile>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMessageBox>
#include <QFileInfo>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSpinBox>
#include <QSplitter>
#include <QStorageInfo>
#include <QSyntaxHighlighter>
#include <QTextBrowser>
#include <QTimeEdit>
#include <QTimer>
#include <QWizardPage>
#include <QtConcurrent>

#include <functional>

using namespace ui;

namespace {

std::string slugify(const QString& text)
{
    QString s = text.normalized(QString::NormalizationForm_KD).toLower();
    QString out;
    for (QChar c : s) {
        if (c.isLetterOrNumber() && c.unicode() < 128)
            out += c;
        else if (!out.endsWith('-') && !out.isEmpty())
            out += '-';
    }
    while (out.endsWith('-'))
        out.chop(1);
    return out.left(48).toStdString();
}

class YamlHighlighter : public QSyntaxHighlighter {
public:
    using QSyntaxHighlighter::QSyntaxHighlighter;

protected:
    void highlightBlock(const QString& text) override
    {
        static const QRegularExpression key(QStringLiteral("^\\s*-?\\s*([A-Za-z_][\\w-]*)\\s*:"));
        static const QRegularExpression str(QStringLiteral("\"[^\"]*\""));
        QTextCharFormat keyFmt, strFmt, commentFmt;
        keyFmt.setForeground(tk().accent);
        keyFmt.setFontWeight(QFont::DemiBold);
        strFmt.setForeground(tk().okFg);
        commentFmt.setForeground(tk().muted);
        commentFmt.setFontItalic(true);
        auto m = key.match(text);
        if (m.hasMatch())
            setFormat(m.capturedStart(1), m.capturedLength(1), keyFmt);
        auto it = str.globalMatch(text);
        while (it.hasNext()) {
            auto s = it.next();
            setFormat(s.capturedStart(), s.capturedLength(), strFmt);
        }
        const int hash = text.indexOf('#');
        if (hash >= 0 && text.left(hash).count('"') % 2 == 0)
            setFormat(hash, text.size() - hash, commentFmt);
    }
};

// Página do assistente com destino configurável.
class FlowPage : public QWizardPage {
public:
    std::function<int()> next;
    std::function<void()> onEnter;
    int nextId() const override { return next ? next() : QWizardPage::nextId(); }
    void initializePage() override
    {
        if (onEnter)
            onEnter();
    }
};

QLabel* title(const QString& t)
{
    return label(t, "h2");
}

} // namespace

// ============================================================================
// AddVaultDialog
// ============================================================================

AddVaultDialog::AddVaultDialog(QWidget* parent, bool network) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Adicionar cofre"));
    resize(720, 680);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(24, 22, 24, 22);
    v->setSpacing(12);
    v->addWidget(title(QStringLiteral("Onde fica o cofre")));
    m_kind = new QComboBox;
    m_kind->addItem(QStringLiteral("Disco desta máquina (interno ou USB)"));
    m_kind->addItem(QStringLiteral("Pasta na rede (NAS ou servidor)"));
    v->addWidget(m_kind);
    m_intro = label(QString(), "muted");
    v->addWidget(m_intro);

    // Disco local.
    m_diskGroup = new QWidget;
    auto* dl = new QVBoxLayout(m_diskGroup);
    dl->setContentsMargins(0, 0, 0, 0);
    m_drive = new QComboBox;
    for (const auto& s : QStorageInfo::mountedVolumes()) {
        if (!s.isValid() || !s.isReady() || s.isReadOnly())
            continue;
        const QString root = QDir::toNativeSeparators(s.rootPath());
        m_drive->addItem(QStringLiteral("%1  %2  (%3, %4)")
                             .arg(root, s.displayName() == s.rootPath() ? QString() : s.displayName(),
                                  QString::fromLatin1(s.fileSystemType()), fmt::bytes(static_cast<std::uint64_t>(s.bytesTotal()))),
                         root);
    }
    dl->addWidget(m_drive);
    v->addWidget(m_diskGroup);

    // Pasta na rede.
    m_netGroup = new QWidget;
    auto* nf = new QFormLayout(m_netGroup);
    nf->setContentsMargins(0, 0, 0, 0);
    nf->setHorizontalSpacing(16);
    m_unc = new QLineEdit;
    m_unc->setPlaceholderText(QStringLiteral("\\\\nas01\\backup\\pdv01"));
    m_netUser = new QLineEdit;
    m_netUser->setPlaceholderText(QStringLiteral("nas01\\synkro  (vazio: conta do computador, em domínio)"));
    m_netPassword = new QLineEdit;
    m_netPassword->setEchoMode(QLineEdit::Password);
    // Rótulos com a mesma largura nos dois formulários, para os campos alinharem.
    auto formLabel = [](const QString& text) {
        auto* l = new QLabel(text);
        l->setMinimumWidth(230);
        return l;
    };
    nf->addRow(formLabel(QStringLiteral("Pasta (caminho UNC)")), m_unc);
    nf->addRow(formLabel(QStringLiteral("Usuário do compartilhamento")), m_netUser);
    nf->addRow(formLabel(QStringLiteral("Senha")), m_netPassword);
    v->addWidget(m_netGroup);

    auto* form = new QFormLayout;
    form->setHorizontalSpacing(16);
    m_nameForm = form;
    m_label = new QLineEdit;
    m_label->setPlaceholderText(QStringLiteral("ex.: HD Externo 1 (Seagate 2 TB)"));
    m_rotation = new QCheckBox(QStringLiteral("Disco de rodízio (sai do local; a ausência é esperada por alguns dias)"));
    form->addRow(formLabel(QStringLiteral("Nome do cofre")), m_label);
    form->addRow(formLabel(QString()), m_rotation);
    m_encrypt = new QCheckBox(QStringLiteral("Cifrar o cofre (AES-256)"));
    m_encrypt->setToolTip(QStringLiteral("Recomendado para discos que saem da loja e para NAS. O conteúdo dos arquivos e dos "
                                         "dumps fica ilegível sem a chave; você recebe uma chave de recuperação para guardar "
                                         "fora do PDV. Só vale para cofres novos, sem backups."));
    form->addRow(formLabel(QString()), m_encrypt);
    v->addLayout(form);

    auto* check = ui::button(QStringLiteral("Verificar"), QStringLiteral("check"));
    connect(check, &QPushButton::clicked, this, [this] { runCheck(); });
    v->addWidget(check, 0, Qt::AlignLeft);

    auto* box = card("insetBox");
    m_findings = new QVBoxLayout(box);
    m_findings->setContentsMargins(16, 14, 16, 14);
    m_findings->setSpacing(10);
    m_findings->addWidget(label(QStringLiteral("Clique em Verificar."), "muted"));
    v->addWidget(box, 1);

    connect(m_kind, &QComboBox::currentIndexChanged, this, [this] { updateKind(); });
    // Mudou o alvo depois de verificar: precisa verificar de novo.
    auto invalidate = [this] {
        m_accepted = false;
        m_initButton->setEnabled(false);
    };
    connect(m_drive, &QComboBox::currentIndexChanged, this, invalidate);
    connect(m_unc, &QLineEdit::textEdited, this, invalidate);
    connect(m_netUser, &QLineEdit::textEdited, this, invalidate);

    m_acceptWarnings = new QCheckBox(QStringLiteral("Li os avisos e quero usar este disco mesmo assim"));
    m_acceptWarnings->hide();
    v->addWidget(m_acceptWarnings);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* cancel = ui::button(QStringLiteral("Cancelar"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    m_initButton = ui::button(QStringLiteral("Inicializar como cofre"), QStringLiteral("shield"), true);
    m_initButton->setEnabled(false);
    connect(m_initButton, &QPushButton::clicked, this, [this] { initialize(); });
    connect(m_acceptWarnings, &QCheckBox::toggled, this, [this](bool on) { m_initButton->setEnabled(m_accepted && on); });
    buttons->addWidget(cancel);
    buttons->addWidget(m_initButton);
    v->addLayout(buttons);
    if (network)
        m_kind->setCurrentIndex(1); // dispara updateKind()
    else
        updateKind();
}

bool AddVaultDialog::network() const
{
    return m_kind->currentIndex() == 1;
}

void AddVaultDialog::updateKind()
{
    const bool net = network();
    m_diskGroup->setVisible(!net);
    m_netGroup->setVisible(net);
    m_nameForm->setRowVisible(m_rotation, !net); // a linha inteira, sem deixar vão
    if (net)
        m_rotation->setChecked(false);
    m_intro->setText(net ? QStringLiteral("Uma pasta num NAS ou servidor protege contra a perda deste PDV inteiro (defeito, roubo, "
                                          "incêndio). Use uma pasta por PDV e um usuário só do Synkro, com permissão de gravação. "
                                          "A senha fica no cofre de segredos desta máquina.")
                         : QStringLiteral("O disco precisa ser diferente do disco de origem e dos outros cofres. Nada é apagado: "
                                          "o Synkro cria apenas a pasta SynkroVault."));
    m_label->setPlaceholderText(net ? QStringLiteral("ex.: NAS do escritório") : QStringLiteral("ex.: HD Externo 1 (Seagate 2 TB)"));
    m_accepted = false;
    m_initButton->setEnabled(false);
    clearLayout(m_findings);
    m_findings->addWidget(label(QStringLiteral("Clique em Verificar."), "muted"));
}

void AddVaultDialog::runCheck()
{
    synkro::vault::CandidateRequest req;
    std::wstring user;
    std::string password;
    if (network()) {
        const std::wstring path = QDir::toNativeSeparators(m_unc->text().trimmed()).toStdWString();
        if (!synkro::win::isUncPath(path)) {
            QMessageBox::information(this, windowTitle(),
                                     QStringLiteral("Informe a pasta como \\\\servidor\\compartilhamento\\pasta. Letras de "
                                                    "unidade mapeadas (Z:) não servem: o serviço do Synkro não as enxerga."));
            return;
        }
        req.candidatePath = synkro::win::normalizeUnc(path);
        user = m_netUser->text().trimmed().toStdWString();
        password = m_netPassword->text().toStdString();
    } else {
        req.candidatePath = m_drive->currentData().toString().toStdWString();
    }
    m_checkedPath = req.candidatePath;
    const auto& set = AppContext::instance().config();
    for (const auto& j : set.jobs) {
        if (j.type == "files")
            req.sourcePaths.push_back(j.files.source);
        else if (j.type == "database")
            for (const auto& f : j.database.sqliteFiles)
                req.sourcePaths.push_back(f);
    }
    for (const auto& obs : AppContext::instance().vaults())
        if (obs.connected)
            req.otherVaults.push_back(obs.location);

    clearLayout(m_findings);
    m_findings->addWidget(label(QStringLiteral("Verificando (inclui um teste de gravação de 1 MB)…"), "muted"));
    m_initButton->setEnabled(false);
    // Resultado da conexão com o compartilhamento (vazio = sem problema).
    auto connectNote = std::make_shared<std::string>();
    auto* watcher = new QFutureWatcher<synkro::vault::CandidateReport>(this);
    connect(watcher, &QFutureWatcher<synkro::vault::CandidateReport>::finished, this, [this, watcher, connectNote] {
        const auto report = watcher->result();
        watcher->deleteLater();
        clearLayout(m_findings);
        if (!connectNote->empty())
            m_findings->addWidget(label(fmt::q(*connectNote), "muted"));
        for (const auto& f : report.findings) {
            auto* row = new QHBoxLayout;
            row->setSpacing(12);
            using S = synkro::vault::CheckSeverity;
            row->addWidget(badge(f.severity == S::Ok ? Tone::Ok : f.severity == S::Blocking ? Tone::Error
                                                                 : f.severity == S::Warning ? Tone::Warn : Tone::Accent),
                           0, Qt::AlignTop);
            auto* txt = new QVBoxLayout;
            txt->setSpacing(0);
            txt->addWidget(label(fmt::q(f.title), "fact"));
            if (!f.detail.empty())
                txt->addWidget(label(fmt::q(f.detail), "muted"));
            row->addLayout(txt, 1);
            m_findings->addLayout(row);
        }
        m_findings->addStretch(1);
        m_accepted = report.accepted();
        m_hasWarnings = report.hasWarnings();
        const bool net = synkro::win::isUncPath(m_checkedPath);
        // Em rede o cofre fica na própria pasta; num disco, na raiz do volume.
        m_mountPoint = net ? m_checkedPath : report.volume.mountPoint;
        if (!net && report.volume.kind == synkro::win::DriveKind::Removable)
            m_rotation->setChecked(true);
        if (m_label->text().isEmpty()) {
            if (net)
                m_label->setText(QStringLiteral("Pasta em rede (%1)").arg(QString::fromStdWString(synkro::win::uncServer(m_checkedPath))));
            else if (!report.volume.label.empty())
                m_label->setText(fmt::q(report.volume.label));
        }
        m_acceptWarnings->setVisible(m_accepted && m_hasWarnings);
        m_acceptWarnings->setChecked(false);
        m_initButton->setEnabled(m_accepted && !m_hasWarnings);
        if (!m_accepted)
            m_findings->addWidget(label(net ? QStringLiteral("Esta pasta não pode ser usada como cofre.")
                                            : QStringLiteral("Este disco não pode ser usado como cofre."),
                                        "fact"));
    });
    auto secret = std::make_shared<std::string>(password);
    watcher->setFuture(QtConcurrent::run([req, user, secret, connectNote] {
        // Conecta na sessão do usuário com a credencial digitada (sem derrubar
        // uma conexão que ele já tenha com o servidor).
        if (!user.empty() && synkro::win::probeSmbServer(synkro::win::uncServer(req.candidatePath))) {
            auto st = synkro::win::connectShare(req.candidatePath, user, *secret, /*replaceConflicting=*/false);
            if (!st && st.error().code == synkro::ErrorCode::AlreadyExists)
                *connectNote = "Esta sessão já tem uma conexão com o servidor usando outro usuário; a verificação usa essa "
                               "conexão. O serviço do Synkro usará o usuário informado.";
            else if (!st)
                *connectNote = "Não foi possível entrar com o usuário informado: " + st.error().message;
        }
        synkro::wipe(*secret);
        return synkro::vault::checkCandidate(req);
    }));
    synkro::wipe(password);
}

void AddVaultDialog::initialize()
{
    const QString name = m_label->text().trimmed();
    if (name.isEmpty()) {
        QMessageBox::information(this, windowTitle(), QStringLiteral("Dê um nome ao cofre."));
        return;
    }
    QString vaultId;
    if (auto existing = synkro::vault::readIdentity(m_mountPoint)) {
        vaultId = existing.value().vaultId;
    } else {
        auto created = synkro::vault::initialize(m_mountPoint, name);
        if (!created) {
            QMessageBox::warning(this, windowTitle(), fmt::q(created.error().message));
            return;
        }
        vaultId = created.value().vaultId;
    }

    // Cifragem: só num cofre ainda sem backups. A chave de recuperação é
    // mostrada uma vez, antes de qualquer outra coisa.
    if (m_encrypt->isChecked() && !synkro::crypto::isEncrypted(m_mountPoint)) {
        auto enabled = synkro::crypto::enableEncryption(m_mountPoint);
        if (!enabled) {
            QMessageBox::warning(this, windowTitle(),
                                 QStringLiteral("O cofre não pôde ser cifrado: %1").arg(fmt::q(enabled.error().message)));
            return;
        }
        RecoveryKeyDialog keyDialog(name, fmt::q(enabled.value().recoveryKey), enabled.value().storedOnMachine,
                                    fmt::q(enabled.value().storeError), this);
        keyDialog.exec();
    }

    auto& ctx = AppContext::instance();
    synkro::config::GlobalConfig g = ctx.config().global;
    std::string base = slugify(name);
    if (base.empty())
        base = "cofre";
    std::string id = base;
    for (int i = 2; g.findVault(id); ++i)
        id = base + "-" + std::to_string(i);
    synkro::config::VaultConfig vc;
    vc.id = id;
    vc.vaultId = vaultId.toStdString();
    vc.label = name.toStdString();
    vc.path = m_mountPoint;
    vc.network = synkro::win::isUncPath(m_mountPoint);
    vc.rotation = !vc.network && m_rotation->isChecked();
    vc.maxAbsenceSeconds = vc.rotation ? 7 * 86400 : 86400;

    // Cofre em rede: a senha vai para o cofre de segredos (nunca para o YAML).
    QString secretWarning;
    if (vc.network && !m_netUser->text().trimmed().isEmpty()) {
        vc.netUser = m_netUser->text().trimmed().toStdString();
        const std::string secretName = "cofre-" + id;
        vc.netPassword = {synkro::config::SecretRef::Kind::Secret, secretName};
        std::string value = m_netPassword->text().toStdString();
        auto st = synkro::SecretStore(synkro::secretsFile()).set(secretName, value);
        synkro::wipe(value);
        if (!st)
            secretWarning = QStringLiteral("\n\nA senha não pôde ser guardada (%1). Cadastre-a em Configurações → Segredos com o "
                                           "nome \"%2\" (requer administrador).")
                                .arg(fmt::q(st.error().message), fmt::q(secretName));
    }
    m_netPassword->clear();

    // Texto proposto para o synkro.yml. Gerado pelo desktop (ou ainda inexistente):
    // regenera inteiro, incluindo o cofre nos padrões se for o primeiro. Editado à
    // mão: insere só o item na lista vaults, preservando comentários e formatação.
    const std::string current = ctx.readConfigFile(L"synkro.yml");
    const bool generated = current.empty() || synkro::config::isPristineGenerated(current);
    std::string proposed;
    bool addedToDefaults = false;
    if (generated) {
        g.vaults.push_back(vc);
        if (g.defaultVaults.empty() || current.empty()) {
            g.defaultVaults.push_back(id);
            addedToDefaults = true;
        }
        proposed = synkro::config::emitGlobal(g);
    } else if (auto inserted = synkro::config::insertVaultIntoGlobalYaml(current, vc)) {
        proposed = inserted.value();
    }
    // Só vale se o resultado for válido e tiver o cofre: senão, vai direto ao editor.
    bool proposedOk = false;
    if (!proposed.empty()) {
        std::vector<synkro::config::Diagnostic> diags;
        const auto parsed = synkro::config::parseGlobalDocument(proposed, L"synkro.yml", diags);
        proposedOk = parsed.findVault(id) != nullptr
                     && std::none_of(diags.begin(), diags.end(),
                                     [](const auto& d) { return d.severity == synkro::config::Severity::Error; });
    }

    QMessageBox ask(QMessageBox::Question, windowTitle(),
                    QStringLiteral("Cofre \"%1\" pronto no disco. Como incluí-lo na configuração?").arg(name), QMessageBox::NoButton,
                    this);
    ask.setInformativeText(
        proposedOk ? QStringLiteral("Automaticamente: o Synkro acrescenta o cofre ao synkro.yml%1.\n"
                                    "Editor YAML: abre o synkro.yml já com o cofre inserido, para você conferir, ajustar "
                                    "(rodízio, limite de ausência, espaço) e salvar.")
                         .arg(generated ? QString() : QStringLiteral(" sem mexer no resto do arquivo"))
                   : QStringLiteral("O synkro.yml está num formato que o Synkro não edita sozinho; abra o editor para incluir "
                                    "o cofre na lista vaults."));
    auto* autoBtn = ask.addButton(QStringLiteral("Adicionar automaticamente"), QMessageBox::AcceptRole);
    auto* editBtn = ask.addButton(QStringLiteral("Revisar no editor YAML"), QMessageBox::ActionRole);
    ask.addButton(QStringLiteral("Depois"), QMessageBox::RejectRole);
    autoBtn->setEnabled(proposedOk);
    ask.setDefaultButton(proposedOk ? autoBtn : editBtn);
    ask.exec();

    if (ask.clickedButton() == autoBtn) {
        if (auto st = ctx.writeConfigFile(L"synkro.yml", proposed); !st) {
            QMessageBox::warning(this, windowTitle(), QStringLiteral("O cofre foi criado no disco (ID %1), mas a configuração "
                                                                    "não pôde ser gravada: %2").arg(vaultId, fmt::q(st.error().message)));
            return;
        }
        QString msg = QStringLiteral("Cofre \"%1\" adicionado à configuração.").arg(name);
        if (!addedToDefaults)
            msg += QStringLiteral("\n\nPara uma tarefa gravar nele, inclua-o nos cofres da tarefa (Tarefas → editar)%1.")
                       .arg(generated ? QString() : QStringLiteral(" ou em defaults.vaults"));
        QMessageBox::information(this, windowTitle(), msg + secretWarning);
    } else if (ask.clickedButton() == editBtn) {
        YamlEditorDialog editor(L"synkro.yml", this, proposedOk ? QString::fromStdString(proposed) : QString(),
                                proposedOk ? QStringLiteral("O cofre \"%1\" (id %2) foi inserido na lista vaults. Confira e "
                                                            "clique em Salvar; Cancelar não altera o arquivo.")
                                                 .arg(name, fmt::q(id))
                                           : QStringLiteral("Inclua o cofre na lista vaults:\n%1")
                                                 .arg(fmt::q(synkro::config::emitVaultEntry(vc))));
        if (editor.exec() != QDialog::Accepted)
            QMessageBox::information(this, windowTitle(),
                                     QStringLiteral("O cofre está pronto no disco (ID %1), mas não entrou na configuração. Para "
                                                    "incluí-lo depois, adicione-o de novo (ele é reaproveitado) ou edite o "
                                                    "synkro.yml.")
                                         .arg(vaultId) + secretWarning);
        else if (!secretWarning.isEmpty())
            QMessageBox::information(this, windowTitle(), secretWarning.trimmed());
    } else {
        QMessageBox::information(this, windowTitle(),
                                 QStringLiteral("O cofre está pronto no disco (ID %1), mas não entrou na configuração. Para "
                                                "incluí-lo depois, adicione o disco de novo (o cofre é reaproveitado).")
                                     .arg(vaultId) + secretWarning);
    }
    accept();
}

// ============================================================================
// RemoveVaultDialog
// ============================================================================

RemoveVaultDialog::RemoveVaultDialog(const std::string& vaultId, const std::wstring& location, QWidget* parent)
    : QDialog(parent), m_vaultId(vaultId), m_location(location)
{
    auto& ctx = AppContext::instance();
    const auto* vc = ctx.config().global.findVault(vaultId);
    m_label = vc ? fmt::q(vc->label) : fmt::q(vaultId);
    setWindowTitle(QStringLiteral("Remover cofre"));
    resize(620, 460);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(24, 22, 24, 22);
    v->setSpacing(12);
    v->addWidget(title(QStringLiteral("Remover o cofre \"%1\"").arg(m_label)));

    auto plan = synkro::config::planVaultRemoval(ctx.config(), vaultId,
                                                 [&ctx](const std::wstring& rel) { return ctx.readConfigFile(rel); });
    QString text;
    bool possible = false;
    if (!plan) {
        text = fmt::q(plan.error().message);
    } else if (!plan.value().possible()) {
        text = QStringLiteral("Não é possível remover agora:");
        for (const auto& b : plan.value().blockers)
            text += QStringLiteral("\n• ") + fmt::q(b);
    } else {
        possible = true;
        m_files = plan.value().files;
        text = QStringLiteral("O cofre sai da configuração: o Synkro deixa de gravar nele.");
        if (!plan.value().affectedJobs.empty()) {
            text += QStringLiteral(" Tarefas que deixam de usá-lo (continuam nos outros cofres):");
            for (const auto& j : plan.value().affectedJobs)
                text += QStringLiteral("\n• ") + fmt::q(j);
        }
        text += QStringLiteral("\n\nOs backups já gravados no disco continuam lá, a menos que você marque a opção abaixo; "
                               "o disco pode ser cadastrado de novo depois.");
    }
    auto* info = label(text, possible ? "muted" : "fact");
    info->setWordWrap(true);
    v->addWidget(info);

    m_deleteData = new QCheckBox(QStringLiteral("Também apagar os backups gravados neste disco (pasta SynkroVault)"));
    m_deleteData->setEnabled(possible && !location.empty());
    v->addWidget(m_deleteData);
    auto* dataHint = label(location.empty()
                               ? QStringLiteral("Disco não conectado: só é possível tirar o cofre da configuração.")
                               : QStringLiteral("Apaga todas as versões de todas as tarefas em %1. Não há como desfazer.")
                                     .arg(QDir::toNativeSeparators(fmt::q(synkro::vault::vaultRoot(location)))),
                           "muted");
    dataHint->setWordWrap(true);
    v->addWidget(dataHint);
    m_confirm = new QLineEdit;
    m_confirm->setPlaceholderText(QStringLiteral("Digite %1 para confirmar").arg(m_label));
    m_confirm->setVisible(false);
    v->addWidget(m_confirm);
    v->addStretch(1);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* cancel = ui::button(QStringLiteral("Cancelar"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    m_removeButton = ui::button(QStringLiteral("Remover cofre"), QStringLiteral("trash"), true);
    m_removeButton->setEnabled(possible);
    buttons->addWidget(cancel);
    buttons->addWidget(m_removeButton);
    v->addLayout(buttons);

    auto update = [this, possible] {
        const bool wipe = m_deleteData->isChecked();
        m_confirm->setVisible(wipe);
        m_removeButton->setText(wipe ? QStringLiteral("Remover e apagar os backups") : QStringLiteral("Remover cofre"));
        m_removeButton->setEnabled(possible && (!wipe || m_confirm->text().trimmed() == m_label));
    };
    connect(m_deleteData, &QCheckBox::toggled, this, update);
    connect(m_confirm, &QLineEdit::textChanged, this, update);
    connect(m_removeButton, &QPushButton::clicked, this, [this] { remove(); });
}

void RemoveVaultDialog::remove()
{
    auto& ctx = AppContext::instance();
    const auto* vc = ctx.config().global.findVault(m_vaultId);
    const std::string uuid = vc ? vc->vaultId : std::string();
    const synkro::config::SecretRef netSecret = vc ? vc->netPassword : synkro::config::SecretRef{};

    // 1. Configuração (tarefas antes do synkro.yml).
    for (const auto& [rel, content] : m_files) {
        if (auto st = ctx.writeConfigFile(rel, content); !st) {
            QMessageBox::warning(this, windowTitle(),
                                 QStringLiteral("Não foi possível gravar %1: %2").arg(fmt::q(rel), fmt::q(st.error().message)));
            ctx.reload();
            return;
        }
    }

    // 2. Segredos que só este cofre usava: a chave de cofre cifrado e a senha do compartilhamento.
    synkro::SecretStore store(synkro::secretsFile());
    if (!uuid.empty())
        (void)store.remove(synkro::crypto::secretNameFor(QString::fromStdString(uuid)));
    if (netSecret.kind == synkro::config::SecretRef::Kind::Secret) {
        bool usedElsewhere = false;
        for (const auto& other : ctx.config().global.vaults)
            usedElsewhere = usedElsewhere || (other.id != m_vaultId && other.netPassword.name == netSecret.name);
        for (const auto& j : ctx.config().jobs)
            for (const auto& s : j.secrets)
                usedElsewhere = usedElsewhere || s.name == netSecret.name;
        if (!usedElsewhere)
            (void)store.remove(netSecret.name);
    }

    // 3. Os backups no disco, se pedido.
    QString result = QStringLiteral("Cofre \"%1\" removido da configuração.").arg(m_label);
    if (m_deleteData->isChecked() && !m_location.empty()) {
        setEnabled(false);
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const std::wstring root = synkro::vault::vaultRoot(m_location);
        auto future = QtConcurrent::run([root] {
            std::uint64_t freed = 0;
            auto st = synkro::ops::removeTree(root, &freed);
            return std::make_pair(st ? std::string() : st.error().message, freed);
        });
        while (!future.isFinished())
            QApplication::processEvents(QEventLoop::AllEvents, 100);
        QApplication::restoreOverrideCursor();
        setEnabled(true);
        const auto [error, freed] = future.result();
        if (error.empty())
            result += QStringLiteral("\nBackups apagados do disco (%1 liberados).").arg(fmt::bytes(freed));
        else
            result += QStringLiteral("\nOs backups não foram apagados por completo: %1. Confira se nenhum backup está "
                                     "rodando e apague a pasta %2 manualmente.")
                          .arg(fmt::q(error), QDir::toNativeSeparators(fmt::q(root)));
    } else if (!m_location.empty()) {
        result += QStringLiteral("\nOs backups continuam no disco, em %1.").arg(QDir::toNativeSeparators(fmt::q(synkro::vault::vaultRoot(m_location))));
    }
    ctx.reload();
    QMessageBox::information(this, windowTitle(), result);
    accept();
}

// ============================================================================
// RecoveryKeyDialog / unlockVaultIfNeeded
// ============================================================================

RecoveryKeyDialog::RecoveryKeyDialog(const QString& vaultLabel, const QString& recoveryKey, bool storedOnMachine,
                                     const QString& storeError, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Chave de recuperação"));
    resize(640, 420);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(24, 22, 24, 22);
    v->setSpacing(12);
    v->addWidget(title(QStringLiteral("Guarde a chave de recuperação")));
    auto* intro = label(QStringLiteral("O cofre \"%1\" está cifrado. Esta chave é a ÚNICA forma de restaurar os backups em "
                                       "outro computador — por exemplo, se este PDV for perdido. Ela não é exibida de novo "
                                       "nem fica gravada no cofre.")
                            .arg(vaultLabel),
                        "muted");
    intro->setWordWrap(true);
    v->addWidget(intro);

    auto* keyLabel = new QLabel(recoveryKey);
    keyLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    keyLabel->setAlignment(Qt::AlignCenter);
    QFont mono(QStringLiteral("Consolas"));
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSize(16);
    mono.setBold(true);
    keyLabel->setFont(mono);
    auto* box = card("insetBox");
    auto* bl = new QVBoxLayout(box);
    bl->setContentsMargins(16, 18, 16, 18);
    bl->addWidget(keyLabel);
    v->addWidget(box);

    auto* tools = new QHBoxLayout;
    auto* copy = ui::button(QStringLiteral("Copiar"), QStringLiteral("copy"));
    connect(copy, &QPushButton::clicked, this, [recoveryKey] {
        // Fora do histórico (Win+V), da sincronização com a nuvem e de
        // monitores de área de transferência: formatos registrados do Windows.
        auto* mime = new QMimeData;
        mime->setText(recoveryKey);
        const QByteArray zero(4, '\0'); // DWORD 0 = "não"
        mime->setData(QStringLiteral("application/x-qt-windows-mime;value=\"CanIncludeInClipboardHistory\""), zero);
        mime->setData(QStringLiteral("application/x-qt-windows-mime;value=\"CanUploadToCloudClipboard\""), zero);
        mime->setData(QStringLiteral("application/x-qt-windows-mime;value=\"ExcludeClipboardContentFromMonitorProcessing\""),
                      zero);
        QApplication::clipboard()->setMimeData(mime);
    });
    auto* save = ui::button(QStringLiteral("Salvar em arquivo…"), QStringLiteral("folder"));
    connect(save, &QPushButton::clicked, this, [this, recoveryKey, vaultLabel] {
        const QString file = QFileDialog::getSaveFileName(this, QStringLiteral("Salvar a chave (use um pendrive, não este PC)"),
                                                          QStringLiteral("chave-recuperacao-synkro.txt"),
                                                          QStringLiteral("Texto (*.txt)"));
        if (file.isEmpty())
            return;
        QFile f(file);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text))
            f.write(QStringLiteral("Synkro — chave de recuperação do cofre \"%1\"\n\n%2\n").arg(vaultLabel, recoveryKey).toUtf8());
    });
    tools->addWidget(copy);
    tools->addWidget(save);
    tools->addStretch(1);
    v->addLayout(tools);

    auto* where = label(storedOnMachine
                            ? QStringLiteral("Nesta máquina a chave fica guardada (DPAPI): os backups agendados funcionam sem "
                                             "pedir nada. Imprima ou anote a chave e guarde-a fora do PDV.")
                            : QStringLiteral("Atenção: a chave NÃO ficou guardada nesta máquina (%1). Abra o Synkro como "
                                             "administrador e use \"synkro-worker vault unlock\" antes do próximo backup.")
                                  .arg(storeError),
                        "muted");
    where->setWordWrap(true);
    v->addWidget(where);
    v->addStretch(1);

    auto* confirm = new QCheckBox(QStringLiteral("Anotei a chave em lugar seguro, fora deste computador"));
    v->addWidget(confirm);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* done = ui::button(QStringLiteral("Concluir"), QStringLiteral("check"), true);
    done->setEnabled(false);
    connect(confirm, &QCheckBox::toggled, done, &QPushButton::setEnabled);
    connect(done, &QPushButton::clicked, this, [this] { QDialog::accept(); });
    buttons->addWidget(done);
    v->addLayout(buttons);
    setProperty("confirmBox", QVariant::fromValue<QObject*>(confirm));
}

void RecoveryKeyDialog::reject()
{
    auto* confirm = qobject_cast<QCheckBox*>(property("confirmBox").value<QObject*>());
    if (confirm && confirm->isChecked()) {
        QDialog::reject();
        return;
    }
    QMessageBox::information(this, windowTitle(),
                             QStringLiteral("Anote a chave de recuperação e marque a confirmação antes de fechar."));
}

bool unlockVaultIfNeeded(QWidget* parent, const std::wstring& location)
{
    if (!synkro::crypto::isEncrypted(location))
        return true;
    auto cipher = synkro::crypto::cipherFor(location);
    if (cipher)
        return true;
    if (cipher.error().code != synkro::ErrorCode::KeyRequired) {
        QMessageBox::warning(parent, QStringLiteral("Cofre cifrado"), fmt::q(cipher.error().message));
        return false;
    }
    QString hint = QStringLiteral("Este cofre é cifrado e a chave não está guardada nesta máquina.\n"
                                  "Digite a chave de recuperação (8 grupos de 4, ex.: ABCD-EFGH-…):");
    for (;;) {
        bool ok = false;
        const QString text = QInputDialog::getText(parent, QStringLiteral("Cofre cifrado"), hint, QLineEdit::Normal,
                                                   QString(), &ok);
        if (!ok || text.trimmed().isEmpty())
            return false;
        std::string typed = text.toStdString();
        QApplication::setOverrideCursor(Qt::WaitCursor);
        auto key = synkro::crypto::unlockWithRecoveryKey(location, typed);
        QApplication::restoreOverrideCursor();
        synkro::wipe(typed);
        if (key) {
            auto id = synkro::vault::readIdentity(location);
            synkro::crypto::rememberKey(id.value().vaultId, key.value());
            synkro::crypto::wipe(key.value());
            return true;
        }
        hint = fmt::q(key.error().message) + QStringLiteral(". Tente de novo:");
    }
}

// ============================================================================
// OpenNetworkVaultDialog
// ============================================================================

OpenNetworkVaultDialog::OpenNetworkVaultDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Abrir cofre em rede"));
    resize(560, 300);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(24, 22, 24, 22);
    v->setSpacing(12);
    v->addWidget(title(QStringLiteral("Cofre numa pasta da rede")));
    v->addWidget(label(QStringLiteral("Funciona também num computador novo, sem configuração: o cofre tem tudo o que é preciso "
                                      "para restaurar. Nada é gravado no NAS."),
                       "muted"));
    auto* form = new QFormLayout;
    m_unc = new QLineEdit;
    m_unc->setPlaceholderText(QStringLiteral("\\\\nas01\\backup\\pdv01"));
    m_user = new QLineEdit;
    m_user->setPlaceholderText(QStringLiteral("vazio: usa a sessão atual do Windows"));
    m_password = new QLineEdit;
    m_password->setEchoMode(QLineEdit::Password);
    form->addRow(QStringLiteral("Pasta (caminho UNC)"), m_unc);
    form->addRow(QStringLiteral("Usuário"), m_user);
    form->addRow(QStringLiteral("Senha"), m_password);
    v->addLayout(form);
    m_status = label(QString(), "muted");
    m_status->setWordWrap(true);
    v->addWidget(m_status);
    v->addStretch(1);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* cancel = ui::button(QStringLiteral("Cancelar"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    m_openButton = ui::button(QStringLiteral("Abrir"), QStringLiteral("folder"), true);
    connect(m_openButton, &QPushButton::clicked, this, [this] { open(); });
    buttons->addWidget(cancel);
    buttons->addWidget(m_openButton);
    v->addLayout(buttons);
}

void OpenNetworkVaultDialog::open()
{
    const std::wstring path = QDir::toNativeSeparators(m_unc->text().trimmed()).toStdWString();
    if (!synkro::win::isUncPath(path)) {
        m_status->setText(QStringLiteral("Informe a pasta como \\\\servidor\\compartilhamento\\pasta."));
        return;
    }
    const std::wstring unc = synkro::win::normalizeUnc(path);
    const std::wstring user = m_user->text().trimmed().toStdWString();
    auto secret = std::make_shared<std::string>(m_password->text().toStdString());
    m_password->clear();
    m_openButton->setEnabled(false);
    m_status->setText(QStringLiteral("Conectando…"));

    struct Outcome {
        std::string error;
        QString label;
    };
    auto* watcher = new QFutureWatcher<Outcome>(this);
    connect(watcher, &QFutureWatcher<Outcome>::finished, this, [this, watcher, unc] {
        const Outcome o = watcher->result();
        watcher->deleteLater();
        m_openButton->setEnabled(true);
        if (!o.error.empty()) {
            m_status->setText(fmt::q(o.error));
            return;
        }
        m_location = unc;
        m_vaultLabel = o.label;
        accept();
    });
    watcher->setFuture(QtConcurrent::run([unc, user, secret]() -> Outcome {
        Outcome o;
        if (auto st = synkro::win::probeSmbServer(synkro::win::uncServer(unc), 5000); !st) {
            synkro::wipe(*secret);
            return {st.error().message, {}};
        }
        if (!user.empty()) {
            auto st = synkro::win::connectShare(unc, user, *secret, /*replaceConflicting=*/false);
            if (!st && st.error().code != synkro::ErrorCode::AlreadyExists) {
                synkro::wipe(*secret);
                return {st.error().message, {}};
            }
        }
        synkro::wipe(*secret);
        auto id = synkro::vault::readIdentity(unc);
        if (!id)
            return {id.error().code == synkro::ErrorCode::NotFound
                        ? "Não há cofre do Synkro nesta pasta (procure a pasta que contém SynkroVault)."
                        : id.error().message,
                    {}};
        o.label = id.value().label;
        return o;
    }));
}

// ============================================================================
// JobWizard
// ============================================================================

JobWizard::JobWizard(QWidget* parent) : QWizard(parent)
{
    setWindowTitle(QStringLiteral("Nova tarefa"));
    setWizardStyle(QWizard::ModernStyle);
    setOption(QWizard::NoBackButtonOnStartPage);
    resize(820, 680);
    setButtonText(QWizard::NextButton, QStringLiteral("Avançar"));
    setButtonText(QWizard::BackButton, QStringLiteral("Voltar"));
    setButtonText(QWizard::FinishButton, QStringLiteral("Criar tarefa"));
    setButtonText(QWizard::CancelButton, QStringLiteral("Cancelar"));

    // 0. Tipo
    auto* start = new FlowPage;
    start->setTitle(QStringLiteral("O que você quer proteger?"));
    auto* sf = new QFormLayout(start);
    m_name = new QLineEdit;
    m_name->setPlaceholderText(QStringLiteral("ex.: Arquivos do PDV"));
    m_type = new QComboBox;
    m_type->addItem(QStringLiteral("Pastas e arquivos (XML fiscais, configuração...)"), "files");
    m_type->addItem(QStringLiteral("Banco de dados (PostgreSQL, MySQL, MariaDB, SQLite)"), "database");
    sf->addRow(QStringLiteral("Nome da tarefa"), m_name);
    sf->addRow(QStringLiteral("Tipo"), m_type);
    sf->addRow(QString(), label(QStringLiteral("Bancos de dados nunca são copiados como arquivos: o Synkro pede ao próprio banco "
                                                "uma cópia consistente."),
                                 "muted"));

    // 1. Arquivos
    auto* files = new FlowPage;
    files->setTitle(QStringLiteral("Pasta de origem"));
    auto* ff = new QFormLayout(files);
    m_source = new QLineEdit;
    auto* browse = ui::button(QStringLiteral("Escolher…"), QStringLiteral("folder"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("Pasta de origem"));
        if (!d.isEmpty())
            m_source->setText(QDir::toNativeSeparators(d));
    });
    auto* srcRow = new QHBoxLayout;
    srcRow->addWidget(m_source, 1);
    srcRow->addWidget(browse);
    m_exclude = new QLineEdit(QStringLiteral("**/*.tmp, Logs/**"));
    m_sentinel = new QLineEdit;
    m_sentinel->setPlaceholderText(QStringLiteral("opcional, ex.: Config/pdv.ini"));
    ff->addRow(QStringLiteral("Pasta"), srcRow);
    ff->addRow(QStringLiteral("Ignorar"), m_exclude);
    ff->addRow(QStringLiteral("Arquivo-sentinela"), m_sentinel);
    ff->addRow(QString(), label(QStringLiteral("O sentinela é um arquivo que sempre existe na origem: se sumir, o Synkro entende "
                                                "que a origem está errada e não altera os cofres."),
                                 "muted"));

    // 2. Banco
    auto* db = new FlowPage;
    db->setTitle(QStringLiteral("Banco de dados"));
    auto* df = new QFormLayout(db);
    m_engine = new QComboBox;
    m_engine->addItem(QStringLiteral("PostgreSQL"), "postgresql");
    m_engine->addItem(QStringLiteral("MySQL"), "mysql");
    m_engine->addItem(QStringLiteral("MariaDB"), "mariadb");
    m_engine->addItem(QStringLiteral("Firebird"), "firebird");
    m_engine->addItem(QStringLiteral("SQL Server"), "sqlserver");
    m_engine->addItem(QStringLiteral("SQLite (arquivo)"), "sqlite");
    m_host = new QLineEdit(QStringLiteral("localhost"));
    m_port = new QSpinBox;
    m_port->setRange(1, 65535);
    m_port->setValue(5432);
    m_user = new QLineEdit(QStringLiteral("synkro_backup"));
    m_password = new QLineEdit;
    m_password->setEchoMode(QLineEdit::Password);
    m_password->setPlaceholderText(QStringLiteral("gravada cifrada no cofre de segredos"));
    m_secretName = new QLineEdit;
    m_databases = new QLineEdit;
    m_databases->setPlaceholderText(QStringLiteral("pdv_vendas, pdv_estoque  —  ou  all"));
    m_sqliteFiles = new QLineEdit;
    m_sqliteFiles->setPlaceholderText(QStringLiteral("C:\\PDV\\dados\\caixa.db; C:\\PDV\\dados\\*.db"));
    auto* test = ui::button(QStringLiteral("Testar conexão"), QStringLiteral("database"));
    m_connectionResult = label(QString(), "muted");
    connect(test, &QPushButton::clicked, this, [this] { testConnection(); });
    connect(m_engine, &QComboBox::currentIndexChanged, this, [this] {
        const QString e = m_engine->currentData().toString();
        const bool server = e != QLatin1String("sqlite");
        for (QWidget* w : {static_cast<QWidget*>(m_host), static_cast<QWidget*>(m_port), static_cast<QWidget*>(m_user),
                           static_cast<QWidget*>(m_password), static_cast<QWidget*>(m_secretName), static_cast<QWidget*>(m_databases)})
            w->setEnabled(server);
        m_sqliteFiles->setEnabled(!server);
        m_port->setValue(e == QLatin1String("postgresql") ? 5432
                         : e == QLatin1String("firebird")  ? 3050
                         : e == QLatin1String("sqlserver") ? 1433
                                                           : 3306);
        if (e == QLatin1String("firebird")) {
            m_user->setText(QStringLiteral("SYSDBA"));
            m_databases->setPlaceholderText(QStringLiteral("C:\\PDV\\dados\\PDV.FDB  (caminho no servidor ou alias)"));
        } else {
            m_databases->setPlaceholderText(QStringLiteral("pdv_vendas, pdv_estoque  —  ou  all"));
        }
    });
    df->addRow(QStringLiteral("Motor"), m_engine);
    df->addRow(QStringLiteral("Servidor"), m_host);
    df->addRow(QStringLiteral("Porta"), m_port);
    df->addRow(QStringLiteral("Usuário"), m_user);
    df->addRow(QStringLiteral("Senha"), m_password);
    df->addRow(QStringLiteral("Nome do segredo"), m_secretName);
    df->addRow(QStringLiteral("Bancos"), m_databases);
    df->addRow(QStringLiteral("Arquivos SQLite"), m_sqliteFiles);
    auto* testRow = new QHBoxLayout;
    testRow->addWidget(test);
    testRow->addWidget(m_connectionResult, 1);
    df->addRow(QString(), testRow);
    m_sqliteFiles->setEnabled(false);

    // 3. Cofres e agendamento
    auto* when = new FlowPage;
    when->setTitle(QStringLiteral("Para onde e quando"));
    auto* wv = new QVBoxLayout(when);
    wv->addWidget(label(QStringLiteral("Cofres"), "fact"));
    m_vaults = new QListWidget;
    m_vaults->setMaximumHeight(130);
    for (const auto& vc : AppContext::instance().config().global.vaults) {
        auto* item = new QListWidgetItem(fmt::q(vc.label), m_vaults);
        item->setData(Qt::UserRole, fmt::q(vc.id));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
    }
    wv->addWidget(m_vaults);
    if (AppContext::instance().config().global.vaults.empty())
        wv->addWidget(label(QStringLiteral("Nenhum cofre cadastrado: adicione um disco em Cofres antes de criar a tarefa."), "fact"));
    wv->addWidget(label(QStringLiteral("Dias e horário"), "fact"));
    auto* daysRow = new QHBoxLayout;
    static const char* names[] = {"Seg", "Ter", "Qua", "Qui", "Sex", "Sáb", "Dom"};
    for (int i = 0; i < 7; ++i) {
        m_days[i] = new QCheckBox(QString::fromUtf8(names[i]));
        m_days[i]->setChecked(i < 6);
        daysRow->addWidget(m_days[i]);
    }
    daysRow->addStretch(1);
    wv->addLayout(daysRow);
    m_time = new QTimeEdit(QTime(22, 0));
    m_time->setDisplayFormat(QStringLiteral("HH:mm"));
    auto* timeRow = new QHBoxLayout;
    timeRow->addWidget(label(QStringLiteral("às")));
    timeRow->addWidget(m_time);
    timeRow->addStretch(1);
    wv->addLayout(timeRow);
    m_onStartup = new QCheckBox(QStringLiteral("Também ao ligar o computador (10 minutos depois)"));
    m_shutdown = new QCheckBox(QStringLiteral("Desligar o computador ao final, somente se tudo der certo"));
    wv->addWidget(m_onStartup);
    wv->addWidget(m_shutdown);
    wv->addStretch(1);

    // 4. Revisão
    auto* review = new FlowPage;
    review->setTitle(QStringLiteral("Revise o que a tarefa vai fazer"));
    auto* rv = new QVBoxLayout(review);
    m_review = new QTextBrowser;
    m_review->setStyleSheet(QStringLiteral("font-family: 'Cascadia Mono', Consolas; font-size: 9.5pt;"));
    rv->addWidget(m_review);
    review->onEnter = [this] { updateReview(); };

    const int pStart = addPage(start);
    m_filesPage = addPage(files);
    m_dbPage = addPage(db);
    const int pWhen = addPage(when);
    const int pReview = addPage(review);
    start->next = [this] { return m_type->currentData().toString() == QLatin1String("files") ? m_filesPage : m_dbPage; };
    files->next = [pWhen] { return pWhen; };
    db->next = [pWhen] { return pWhen; };
    when->next = [pReview] { return pReview; };
    review->next = [] { return -1; };
    connect(m_name, &QLineEdit::textChanged, this, [this](const QString& t) {
        m_secretName->setText(QString::fromStdString(slugify(t)));
    });
    (void)pStart;
}

synkro::config::JobConfig JobWizard::buildJob() const
{
    synkro::config::JobConfig j;
    j.id = slugify(m_name->text());
    j.name = m_name->text().trimmed().toStdString();
    j.type = m_type->currentData().toString().toStdString();
    for (int i = 0; i < m_vaults->count(); ++i)
        if (m_vaults->item(i)->checkState() == Qt::Checked)
            j.vaults.push_back(m_vaults->item(i)->data(Qt::UserRole).toString().toStdString());

    synkro::config::ScheduleTrigger t;
    std::vector<int> days;
    for (int i = 0; i < 7; ++i)
        if (m_days[i]->isChecked())
            days.push_back(i + 1);
    t.atMinutes = m_time->time().hour() * 60 + m_time->time().minute();
    if (days.size() == 7) {
        t.kind = synkro::config::TriggerKind::Daily;
    } else {
        t.kind = synkro::config::TriggerKind::Weekly;
        t.days = days;
    }
    if (!days.empty())
        j.schedule.push_back(t);
    if (m_onStartup->isChecked()) {
        synkro::config::ScheduleTrigger boot;
        boot.kind = synkro::config::TriggerKind::OnStartup;
        boot.delaySeconds = 600;
        j.schedule.push_back(boot);
    }
    j.shutdownAfter = m_shutdown->isChecked();

    if (j.type == "files") {
        j.files.source = QDir::toNativeSeparators(m_source->text().trimmed()).toStdWString();
        for (const auto& p : m_exclude->text().split(',', Qt::SkipEmptyParts))
            j.files.exclude.push_back(p.trimmed().toStdWString());
        j.files.sentinelFile = m_sentinel->text().trimmed().toStdWString();
    } else {
        auto& d = j.database;
        d.engine = m_engine->currentData().toString().toStdString();
        if (d.engine == "sqlite") {
            for (const auto& f : m_sqliteFiles->text().split(';', Qt::SkipEmptyParts))
                d.sqliteFiles.push_back(QDir::toNativeSeparators(f.trimmed()).toStdWString());
        } else {
            d.host = m_host->text().trimmed().toStdString();
            d.port = m_port->value();
            d.user = m_user->text().trimmed().toStdString();
            d.password = {synkro::config::SecretRef::Kind::Secret, m_secretName->text().trimmed().toStdString()};
            const QString dbs = m_databases->text().trimmed();
            if (dbs == QLatin1String("all")) {
                d.allDatabases = true;
            } else {
                for (const auto& n : dbs.split(',', Qt::SkipEmptyParts)) {
                    const std::string entry = n.trimmed().toStdString();
                    if (d.engine == "firebird") {
                        // Nome lógico = arquivo sem extensão; o caminho vai como o servidor o enxerga.
                        const QFileInfo fi(n.trimmed());
                        d.databases.push_back({fi.completeBaseName().toStdString(), {}, entry});
                    } else {
                        d.databases.push_back({entry, {}, {}});
                    }
                }
            }
            if (d.engine == "sqlserver")
                d.sslMode = synkro::config::isLocalDatabaseHost(d.host) ? "optional" : "mandatory";
        }
    }
    return j;
}

void JobWizard::testConnection()
{
    const auto job = buildJob();
    if (job.database.engine == "sqlite") {
        m_connectionResult->setText(QStringLiteral("SQLite não precisa de conexão."));
        return;
    }
    synkro::db::Connection c = synkro::db::connectionFor(job.database, m_password->text().toStdString());
    c.connectTimeoutSeconds = 10;
    if (job.database.engine == "postgresql" && !synkro::config::isLocalDatabaseHost(job.database.host))
        c.sslMode = "require";
    m_connectionResult->setText(QStringLiteral("testando…"));
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        m_connectionResult->setText(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([job, c]() -> QString {
        auto dir = synkro::db::PrivateDirectory::create(
            synkro::win::joinPath(synkro::stagingDirectory(), L"teste-" + synkro::newRunId()));
        if (!dir)
            return fmt::q(dir.error().message);
        synkro::db::DumpContext ctx;
        ctx.workDir = dir.value().path();
        ctx.retries = 0;
        auto dbs = synkro::db::listServerDatabases(job.database, c, ctx);
        if (!dbs)
            return QStringLiteral("✗ ") + fmt::q(dbs.error().message);
        QStringList names;
        for (const auto& n : dbs.value())
            names << fmt::q(n);
        return QStringLiteral("✓ conectado. Bancos: ") + names.join(QStringLiteral(", "));
    }));
}

void JobWizard::updateReview()
{
    const auto job = buildJob();
    auto set = AppContext::instance().config();
    std::vector<synkro::config::Diagnostic> diags;
    const std::string yaml = synkro::config::emitJob(job);
    const auto parsed = synkro::config::parseJobDocument(yaml, L"jobs\\" + synkro::win::fromUtf8(job.id) + L".yml", set.global, diags);
    QString text;
    for (const auto& d : diags)
        text += fmt::q(synkro::config::formatDiagnostic(d)) + "\n";
    if (!text.isEmpty())
        text += "\n";
    set.jobs.push_back(parsed);
    text += fmt::q(synkro::config::describeJob(set, parsed));
    m_review->setPlainText(text);
}

void JobWizard::accept()
{
    const auto job = buildJob();
    if (job.id.empty()) {
        QMessageBox::warning(this, windowTitle(), QStringLiteral("Dê um nome à tarefa."));
        return;
    }
    auto& ctx = AppContext::instance();
    if (ctx.config().findJob(job.id)) {
        QMessageBox::warning(this, windowTitle(), QStringLiteral("Já existe uma tarefa com o id \"%1\".").arg(fmt::q(job.id)));
        return;
    }
    const std::string yaml = synkro::config::emitJob(job);
    std::vector<synkro::config::Diagnostic> diags;
    (void)synkro::config::parseJobDocument(yaml, L"jobs\\x.yml", ctx.config().global, diags);
    for (const auto& d : diags) {
        if (d.severity == synkro::config::Severity::Error) {
            QMessageBox::warning(this, windowTitle(), QStringLiteral("Corrija antes de criar:\n") + fmt::q(synkro::config::formatDiagnostic(d)));
            return;
        }
    }
    if (job.type == "database" && job.database.engine != "sqlite" && !m_password->text().isEmpty()) {
        std::string pw = m_password->text().toStdString();
        auto st = synkro::SecretStore(synkro::secretsFile()).set(job.database.password.name, pw);
        synkro::wipe(pw);
        if (!st) {
            QMessageBox::warning(this, windowTitle(), QStringLiteral("Não foi possível gravar o segredo: ") + fmt::q(st.error().message));
            return;
        }
    }
    if (auto st = ctx.writeConfigFile(L"jobs\\" + synkro::win::fromUtf8(job.id) + L".yml", yaml); !st) {
        QMessageBox::warning(this, windowTitle(), fmt::q(st.error().message));
        return;
    }
    QMessageBox::information(this, windowTitle(),
                             QStringLiteral("Tarefa criada. Para agendá-la no Windows, use Configurações › Aplicar e agendar."));
    QWizard::accept();
}

// ============================================================================
// YamlEditorDialog
// ============================================================================

YamlEditorDialog::YamlEditorDialog(const std::wstring& relativePath, QWidget* parent, const QString& proposedText,
                                   const QString& notice)
    : QDialog(parent), m_path(relativePath), m_isGlobal(synkro::win::foldCase(relativePath) == L"SYNKRO.YML")
{
    setWindowTitle(QStringLiteral("Editor YAML — ") + fmt::q(relativePath));
    resize(1180, 760);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(20, 18, 20, 18);
    if (!notice.isEmpty()) {
        auto* n = label(notice, "fact");
        n->setWordWrap(true);
        v->addWidget(n);
    }
    v->addWidget(label(QStringLiteral("As alterações são validadas enquanto você digita; o arquivo só é salvo sem erros."), "muted"));

    auto* split = new QSplitter;
    m_editor = new QPlainTextEdit;
    m_editor->setProperty("role", "code");
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setPlainText(proposedText.isEmpty() ? QString::fromStdString(AppContext::instance().readConfigFile(relativePath))
                                                  : proposedText);
    new YamlHighlighter(m_editor->document());
    split->addWidget(m_editor);

    auto* side = new QWidget;
    auto* sv = new QVBoxLayout(side);
    sv->setContentsMargins(12, 0, 0, 0);
    sv->addWidget(label(QStringLiteral("PROBLEMAS"), "caption"));
    m_diagnostics = new QListWidget;
    m_diagnostics->setMaximumHeight(160);
    connect(m_diagnostics, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        const int line = item->data(Qt::UserRole).toInt();
        if (line <= 0)
            return;
        QTextCursor c(m_editor->document()->findBlockByNumber(line - 1));
        m_editor->setTextCursor(c);
        m_editor->setFocus();
    });
    sv->addWidget(m_diagnostics);
    sv->addWidget(label(m_isGlobal ? QStringLiteral("RESUMO") : QStringLiteral("O QUE ESTA TAREFA VAI FAZER"), "caption"));
    m_plan = new QTextBrowser;
    m_plan->setStyleSheet(QStringLiteral("font-family: 'Cascadia Mono', Consolas; font-size: 9pt;"));
    sv->addWidget(m_plan, 1);
    split->addWidget(side);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    v->addWidget(split, 1);

    auto* row = new QHBoxLayout;
    row->addStretch(1);
    auto* cancel = ui::button(QStringLiteral("Cancelar"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    m_save = ui::button(QStringLiteral("Salvar"), QStringLiteral("check"), true);
    connect(m_save, &QPushButton::clicked, this, [this] { save(); });
    row->addWidget(cancel);
    row->addWidget(m_save);
    v->addLayout(row);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(350);
    connect(m_debounce, &QTimer::timeout, this, [this] { validate(); });
    connect(m_editor, &QPlainTextEdit::textChanged, m_debounce, qOverload<>(&QTimer::start));
    validate();
}

void YamlEditorDialog::validate()
{
    const std::string text = m_editor->toPlainText().toStdString();
    std::vector<synkro::config::Diagnostic> diags;
    auto set = AppContext::instance().config();
    QString plan;
    if (m_isGlobal) {
        const auto g = synkro::config::parseGlobalDocument(text, m_path, diags);
        plan = QStringLiteral("%1 cofre(s) declarado(s).\n").arg(g.vaults.size());
        for (const auto& v : g.vaults)
            plan += QStringLiteral("  • %1 (%2)%3\n").arg(fmt::q(v.label), fmt::q(v.id), v.rotation ? QStringLiteral(" · rodízio") : QString());
    } else {
        const auto job = synkro::config::parseJobDocument(text, m_path, set.global, diags);
        set.jobs.erase(std::remove_if(set.jobs.begin(), set.jobs.end(), [&](const auto& j) { return j.id == job.id; }), set.jobs.end());
        set.jobs.push_back(job);
        plan = fmt::q(synkro::config::describeJob(set, job));
    }
    m_diagnostics->clear();
    m_hasErrors = false;
    for (const auto& d : diags) {
        m_hasErrors = m_hasErrors || d.severity == synkro::config::Severity::Error;
        auto* item = new QListWidgetItem(fmt::q(synkro::config::formatDiagnostic(d)), m_diagnostics);
        item->setData(Qt::UserRole, d.line);
        item->setForeground(d.severity == synkro::config::Severity::Error ? tk().errFg : tk().warnFg);
    }
    if (diags.empty())
        new QListWidgetItem(QStringLiteral("✓ nenhum problema"), m_diagnostics);
    m_plan->setPlainText(plan);
    m_save->setEnabled(!m_hasErrors);
}

void YamlEditorDialog::save()
{
    if (auto st = AppContext::instance().writeConfigFile(m_path, m_editor->toPlainText().toStdString()); !st) {
        QMessageBox::warning(this, windowTitle(), fmt::q(st.error().message));
        return;
    }
    accept();
}

// ============================================================================
// SecretDialog
// ============================================================================

SecretDialog::SecretDialog(const QString& name, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Cadastrar segredo"));
    resize(460, 220);
    auto* f = new QFormLayout(this);
    f->setContentsMargins(22, 20, 22, 20);
    m_name = new QLineEdit(name);
    m_name->setPlaceholderText(QStringLiteral("ex.: pg-pdv"));
    m_value = new QLineEdit;
    m_value->setEchoMode(QLineEdit::Password);
    m_confirm = new QLineEdit;
    m_confirm->setEchoMode(QLineEdit::Password);
    f->addRow(QStringLiteral("Nome"), m_name);
    f->addRow(QStringLiteral("Valor"), m_value);
    f->addRow(QStringLiteral("Confirmar"), m_confirm);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    box->button(QDialogButtonBox::Save)->setText(QStringLiteral("Gravar"));
    box->button(QDialogButtonBox::Save)->setProperty("role", "primary");
    box->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Cancelar"));
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    f->addRow(box);
}

QString SecretDialog::name() const
{
    return m_name->text().trimmed();
}

void SecretDialog::accept()
{
    if (m_value->text() != m_confirm->text()) {
        QMessageBox::warning(this, windowTitle(), QStringLiteral("Os valores não conferem."));
        return;
    }
    std::string value = m_value->text().toStdString();
    auto st = synkro::SecretStore(synkro::secretsFile()).set(name().toStdString(), value);
    synkro::wipe(value);
    m_value->clear();
    m_confirm->clear();
    if (!st) {
        QMessageBox::warning(this, windowTitle(), fmt::q(st.error().message));
        return;
    }
    QDialog::accept();
}

// ============================================================================
// ImportConfigDialog
// ============================================================================

ImportConfigDialog::ImportConfigDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Importar configuração"));
    resize(760, 680);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(24, 22, 24, 22);
    v->setSpacing(12);
    v->addWidget(title(QStringLiteral("Importar de outro PDV")));
    v->addWidget(label(QStringLiteral("O pacote traz as tarefas e as preferências, sem senhas e sem a identidade dos discos. "
                                      "Ligue cada cofre do pacote a um disco desta máquina já preparado como cofre "
                                      "(Cofres → Adicionar cofre). Nada é gravado sem uma simulação válida."),
                       "muted"));

    auto* pick = ui::button(QStringLiteral("Escolher pacote…"), QStringLiteral("folder"));
    connect(pick, &QPushButton::clicked, this, [this] { choosePackage(); });
    m_source = label(QStringLiteral("Nenhum pacote escolhido."), "muted");
    auto* top = new QHBoxLayout;
    top->addWidget(pick);
    top->addWidget(m_source, 1);
    v->addLayout(top);

    auto* box = card("insetBox");
    m_vaultRows = new QVBoxLayout(box);
    m_vaultRows->setContentsMargins(16, 14, 16, 14);
    m_vaultRows->setSpacing(8);
    m_vaultRows->addWidget(label(QStringLiteral("Os cofres do pacote aparecem aqui depois de escolher o pacote."), "muted"));
    v->addWidget(box);

    m_replace = new QCheckBox(QStringLiteral("Substituir tarefas que já existem nesta máquina com conteúdo diferente"));
    connect(m_replace, &QCheckBox::toggled, this, [this] { m_import->setEnabled(false); });
    v->addWidget(m_replace);

    m_report = new QTextBrowser;
    m_report->setPlaceholderText(QStringLiteral("O resultado da simulação aparece aqui."));
    v->addWidget(m_report, 1);

    auto* buttons = new QHBoxLayout;
    auto* simulateBtn = ui::button(QStringLiteral("Simular"), QStringLiteral("check"));
    connect(simulateBtn, &QPushButton::clicked, this, [this] { simulate(); });
    auto* cancel = ui::button(QStringLiteral("Fechar"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    m_import = ui::button(QStringLiteral("Importar (administrador)"), QStringLiteral("shield"), true);
    m_import->setEnabled(false);
    connect(m_import, &QPushButton::clicked, this, [this] { runImport(); });
    buttons->addWidget(simulateBtn);
    buttons->addStretch(1);
    buttons->addWidget(cancel);
    buttons->addWidget(m_import);
    v->addLayout(buttons);
}

void ImportConfigDialog::choosePackage()
{
    const QString file = QFileDialog::getOpenFileName(this, QStringLiteral("Pacote de configuração"), QString(),
                                                      QStringLiteral("Pacote do Synkro (*.yml *.yaml)"));
    if (file.isEmpty())
        return;
    auto text = synkro::win::readWholeFile(QDir::toNativeSeparators(file).toStdWString(), 16u << 20);
    auto pkg = text ? synkro::config::parsePackage(text.value()) : synkro::Result<synkro::config::ConfigPackage>(text.error());
    if (!pkg) {
        QMessageBox::warning(this, windowTitle(), fmt::q(pkg.error().message));
        return;
    }
    m_file = QDir::toNativeSeparators(file);
    m_import->setEnabled(false);
    m_report->clear();
    const auto& p = pkg.value();
    m_source->setText(QStringLiteral("%1 — exportado de %2 em %3 · %4 arquivo(s)")
                          .arg(QFileInfo(file).fileName(), fmt::q(p.exportedFrom), fmt::q(p.exportedAt))
                          .arg(p.files.size()));

    // Cofres do Synkro conectados agora (com a identidade lida do vault.json).
    struct Connected {
        QString root, label;
    };
    std::vector<Connected> connected;
    for (const auto& s : QStorageInfo::mountedVolumes()) {
        if (!s.isValid() || !s.isReady())
            continue;
        const QString root = QDir::toNativeSeparators(s.rootPath());
        if (auto id = synkro::vault::readIdentity(root.toStdWString()))
            connected.push_back({root, id.value().label});
    }

    clearLayout(m_vaultRows);
    m_vaultCombos.clear();
    m_vaultRows->addWidget(label(QStringLiteral("Cofres do pacote"), "fact"));
    const auto& here = AppContext::instance().config().global;
    for (const auto& role : p.vaults) {
        auto* row = new QHBoxLayout;
        row->addWidget(label(QStringLiteral("%1  (%2)").arg(fmt::q(role.label), fmt::q(role.id))), 1);
        if (here.findVault(role.id)) {
            row->addWidget(label(QStringLiteral("já existe nesta máquina"), "muted"));
        } else {
            auto* combo = new QComboBox;
            combo->addItem(QStringLiteral("(não ligar)"), QString());
            for (const auto& c : connected)
                combo->addItem(QStringLiteral("%1  %2").arg(c.root, c.label), c.root);
            // Sugere o disco com o mesmo nome de cofre.
            for (int i = 1; i < combo->count(); ++i)
                if (combo->itemText(i).endsWith(fmt::q(role.label)))
                    combo->setCurrentIndex(i);
            connect(combo, &QComboBox::currentIndexChanged, this, [this] { m_import->setEnabled(false); });
            row->addWidget(combo, 1);
            m_vaultCombos.emplace_back(role.id, combo);
        }
        m_vaultRows->addLayout(row);
    }
    if (connected.empty())
        m_vaultRows->addWidget(label(QStringLiteral("Nenhum cofre conectado. Prepare os discos em Cofres → Adicionar cofre."),
                                     "muted"));
}

std::map<std::string, std::wstring> ImportConfigDialog::bindings() const
{
    std::map<std::string, std::wstring> out;
    for (const auto& [id, combo] : m_vaultCombos) {
        const QString root = combo->currentData().toString();
        if (!root.isEmpty())
            out[id] = root.toStdWString();
    }
    return out;
}

bool ImportConfigDialog::simulate()
{
    if (m_file.isEmpty()) {
        m_report->setPlainText(QStringLiteral("Escolha um pacote."));
        return false;
    }
    auto raw = synkro::win::readWholeFile(m_file.toStdWString(), 16u << 20);
    if (!raw) {
        m_report->setPlainText(fmt::q(raw.error().message));
        return false;
    }
    auto pkg = synkro::config::parsePackage(raw.value());
    if (!pkg) {
        m_report->setPlainText(fmt::q(pkg.error().message));
        return false;
    }
    synkro::config::ImportOptions opt;
    opt.configDir = AppContext::instance().configDir();
    opt.bindings = bindings();
    opt.replaceJobs = m_replace->isChecked();
    opt.dryRun = true;
    auto r = synkro::config::importPackage(pkg.value(), opt);
    if (!r) {
        m_report->setPlainText(QStringLiteral("✗ ") + fmt::q(r.error().message));
        m_import->setEnabled(false);
        return false;
    }
    QString text = QStringLiteral("✓ Simulação válida. Ao importar:\n");
    for (const auto& w : r.value().written)
        text += QStringLiteral("  • grava %1\n").arg(fmt::q(w));
    for (const auto& k : r.value().kept)
        text += QStringLiteral("  • mantém %1\n").arg(fmt::q(k));
    for (const auto& w : r.value().warnings)
        text += QStringLiteral("  ⚠ %1\n").arg(fmt::q(w));
    for (const auto& d : r.value().diagnostics)
        if (d.severity == synkro::config::Severity::Warning)
            text += QStringLiteral("  ⚠ %1\n").arg(fmt::q(synkro::config::formatDiagnostic(d)));
    if (!r.value().missingSecrets.empty()) {
        text += QStringLiteral("\nDepois, cadastre em Configurações → Segredos:");
        for (const auto& s : r.value().missingSecrets)
            text += QStringLiteral(" %1").arg(fmt::q(s));
        text += '\n';
    }
    if (r.value().written.empty())
        text += QStringLiteral("\nNada a gravar: esta máquina já tem essa configuração.");
    m_report->setPlainText(text);
    m_import->setEnabled(!r.value().written.empty());
    return true;
}

void ImportConfigDialog::runImport()
{
    if (!simulate())
        return;
    QStringList args = {QStringLiteral("config"), QStringLiteral("import"), QStringLiteral("--from"), m_file,
                        QStringLiteral("--config-dir"), fmt::q(AppContext::instance().configDir())};
    for (const auto& [id, root] : bindings())
        args << QStringLiteral("--bind") << fmt::q(id) + QLatin1Char('=') + fmt::q(root);
    if (m_replace->isChecked())
        args << QStringLiteral("--replace");
    auto r = AppContext::instance().runWorkerElevated(args);
    if (!r) {
        QMessageBox::warning(this, windowTitle(), fmt::q(r.error().message));
        return;
    }
    if (r.value() != 0) {
        QMessageBox::warning(this, windowTitle(),
                             QStringLiteral("O worker terminou com código %1. Rode \"synkro-worker config import --from "
                                            "<pacote> --dry-run\" num prompt de administrador para ver os detalhes.")
                                 .arg(r.value()));
        return;
    }
    AppContext::instance().reload();
    QMessageBox::information(this, windowTitle(),
                             QStringLiteral("Configuração importada. Cadastre os segredos que faltam e use \"Aplicar e "
                                            "agendar\" para registrar as tarefas."));
    accept();
}

// ============================================================================
// RunDialog
// ============================================================================

RunDialog::RunDialog(const QString& jobId, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Executando ") + jobId);
    resize(780, 520);
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(20, 18, 20, 18);
    m_status = label(QStringLiteral("Iniciando…"), "h2");
    v->addWidget(m_status);
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setProperty("role", "code");
    v->addWidget(m_log, 1);
    m_close = ui::button(QStringLiteral("Fechar"));
    m_close->setEnabled(false);
    connect(m_close, &QPushButton::clicked, this, &QDialog::accept);
    v->addWidget(m_close, 0, Qt::AlignRight);

    // Se a tarefa está registrada no Agendador, roda como SYSTEM (com VSS).
    if (synkro::win::runTaskNow(L"job-" + jobId.toStdWString())) {
        m_status->setText(QStringLiteral("Iniciada pelo Agendador do Windows (como SYSTEM)"));
        m_log->setPlainText(QStringLiteral("Acompanhe o resultado no Painel e no Histórico em alguns instantes."));
        m_close->setEnabled(true);
        return;
    }

    m_process.setProgram(AppContext::instance().workerPath());
    m_process.setArguments({QStringLiteral("backup"), QStringLiteral("--job"), jobId, QStringLiteral("--trigger"),
                            QStringLiteral("manual"), QStringLiteral("--no-shutdown"), QStringLiteral("--config-dir"),
                            fmt::q(AppContext::instance().configDir())});
    m_process.setProcessChannelMode(QProcess::MergedChannels);
    connect(&m_process, &QProcess::readyRead, this, [this] {
        m_log->moveCursor(QTextCursor::End);
        m_log->insertPlainText(QString::fromUtf8(m_process.readAll()));
        m_log->moveCursor(QTextCursor::End);
    });
    connect(&m_process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        static const QHash<int, QString> texts = {{0, QStringLiteral("Concluída com sucesso")},
                                                  {1, QStringLiteral("Concluída com avisos (degradada)")},
                                                  {2, QStringLiteral("Falhou")},
                                                  {3, QStringLiteral("Bloqueada por uma proteção")},
                                                  {4, QStringLiteral("A tarefa já estava em execução")}};
        m_status->setText(texts.value(code, QStringLiteral("Terminou com código %1").arg(code)));
        m_close->setEnabled(true);
        AppContext::instance().reload();
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        m_status->setText(QStringLiteral("Não foi possível iniciar o synkro-worker"));
        m_log->appendPlainText(m_process.errorString());
        m_close->setEnabled(true);
    });
    m_status->setText(QStringLiteral("Em execução…"));
    m_process.start();
}
