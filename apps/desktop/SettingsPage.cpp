#include "AppContext.h"
#include "Dialogs.h"
#include "Format.h"
#include "Pages.h"
#include "Theme.h"

#include "config/ConfigLoader.h"
#include "config/ConfigPackage.h"
#include "core/Paths.h"
#include "platform/windows/Snapshot.h"
#include "platform/windows/Win32.h"
#include "secrets/SecretStore.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>
#include <QSysInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QRadioButton>
#include <QSettings>
#include <QUrl>

using namespace ui;

namespace {

const QString kRunKeyUser = QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
const QString kRunKeyMachine = QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
const QString kRunValue = QStringLiteral("Synkro");

} // namespace

bool autostartForAllUsers()
{
    return QSettings(kRunKeyMachine, QSettings::NativeFormat).contains(kRunValue);
}

bool autostartForCurrentUser()
{
    return QSettings(kRunKeyUser, QSettings::NativeFormat).contains(kRunValue);
}

bool setAutostartForCurrentUser(bool on)
{
    QSettings run(kRunKeyUser, QSettings::NativeFormat);
    if (on)
        run.setValue(kRunValue, QStringLiteral("\"%1\" --tray").arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath())));
    else
        run.remove(kRunValue);
    run.sync();
    return run.status() == QSettings::NoError;
}

SettingsPage::SettingsPage() : Page(QStringLiteral("Configurações"), QStringLiteral("Aparência, segredos e configuração do Synkro"))
{
}

void SettingsPage::refresh()
{
    clearLayout(body());
    auto& ctx = AppContext::instance();

    // ---- Aparência --------------------------------------------------------------
    auto* look = card();
    auto* lv = new QVBoxLayout(look);
    lv->setContentsMargins(22, 20, 22, 20);
    lv->setSpacing(10);
    lv->addWidget(label(QStringLiteral("Aparência"), "h2"));
    lv->addWidget(label(QStringLiteral("\"Sistema\" acompanha o tema do Windows automaticamente."), "muted"));
    auto* row = new QHBoxLayout;
    auto* group = new QButtonGroup(look);
    for (ThemeMode m : {ThemeMode::Light, ThemeMode::Dark, ThemeMode::System}) {
        auto* rb = new QRadioButton(Theme::modeName(m));
        rb->setChecked(Theme::instance().mode() == m);
        group->addButton(rb);
        connect(rb, &QRadioButton::toggled, this, [m](bool on) {
            if (on)
                Theme::instance().setMode(m);
        });
        row->addWidget(rb);
    }
    row->addStretch(1);
    lv->addLayout(row);

    // Início automático na bandeja (por usuário; o instalador pode ligar para todos).
    lv->addSpacing(6);
    lv->addWidget(label(QStringLiteral("Inicialização"), "fact"));
    auto* autostart = new QCheckBox(QStringLiteral("Abrir o Synkro na bandeja ao entrar no Windows"));
    const bool forAll = autostartForAllUsers();
    autostart->setChecked(forAll || autostartForCurrentUser());
    autostart->setEnabled(!forAll);
    connect(autostart, &QCheckBox::toggled, this, [this](bool on) {
        if (!setAutostartForCurrentUser(on))
            QMessageBox::warning(this, QStringLiteral("Inicialização"),
                                 QStringLiteral("Não foi possível alterar a inicialização automática."));
    });
    lv->addWidget(autostart);
    lv->addWidget(label(forAll ? QStringLiteral("Ligado para todos os usuários pelo instalador.")
                               : QStringLiteral("Os alertas (backup atrasado, cofre ausente, falhas) aparecem na bandeja "
                                                "mesmo com a janela fechada."),
                        "muted"));
    body()->addWidget(look);

    // ---- Segredos ----------------------------------------------------------------
    auto* sec = card();
    auto* sv = new QVBoxLayout(sec);
    sv->setContentsMargins(22, 20, 22, 20);
    sv->setSpacing(10);
    sv->addWidget(label(QStringLiteral("Segredos"), "h2"));
    sv->addWidget(label(QStringLiteral("Senhas dos bancos de dados, cifradas pelo Windows (DPAPI) em %1. O YAML guarda só o "
                                       "nome do segredo; o valor nunca aparece na tela depois de gravado.")
                            .arg(fmt::q(synkro::secretsFile())),
                        "muted"));
    m_secrets = new QListWidget;
    m_secrets->setMinimumHeight(120);
    sv->addWidget(m_secrets);
    auto* srow = new QHBoxLayout;
    auto* add = button(QStringLiteral("Cadastrar ou alterar…"), QStringLiteral("key"));
    auto* del = button(QStringLiteral("Remover"), QStringLiteral("x"));
    del->setProperty("role", "danger");
    connect(add, &QPushButton::clicked, this, [this] {
        SecretDialog dlg(QString(), this);
        if (dlg.exec() == QDialog::Accepted)
            refreshSecrets();
    });
    connect(del, &QPushButton::clicked, this, [this] {
        auto* item = m_secrets->currentItem();
        if (!item)
            return;
        if (QMessageBox::question(this, QStringLiteral("Remover segredo"),
                                  QStringLiteral("Remover o segredo \"%1\"? Tarefas que o usam vão falhar até ele ser "
                                                 "cadastrado de novo.").arg(item->text()))
            != QMessageBox::Yes)
            return;
        auto st = synkro::SecretStore(synkro::secretsFile()).remove(item->text().toStdString());
        if (!st)
            QMessageBox::warning(this, QStringLiteral("Segredos"), fmt::q(st.error().message));
        refreshSecrets();
    });
    srow->addWidget(add);
    srow->addWidget(del);
    srow->addStretch(1);
    sv->addLayout(srow);
    body()->addWidget(sec);
    refreshSecrets();

    // ---- Configuração -------------------------------------------------------------
    auto* cfg = card();
    auto* cv = new QVBoxLayout(cfg);
    cv->setContentsMargins(22, 20, 22, 20);
    cv->setSpacing(10);
    cv->addWidget(label(QStringLiteral("Configuração"), "h2"));
    cv->addWidget(label(QStringLiteral("Pasta: %1").arg(fmt::q(ctx.configDir())), "muted"));
    const auto& set = ctx.config();
    cv->addWidget(label(QStringLiteral("%1 tarefa(s), %2 cofre(s) · %3 erro(s), %4 aviso(s)")
                            .arg(set.jobs.size()).arg(set.global.vaults.size()).arg(set.errorCount()).arg(set.warningCount())));
    for (const auto& d : set.diagnostics)
        cv->addWidget(label(fmt::q(synkro::config::formatDiagnostic(d)), "muted"));

    auto* crow = new QHBoxLayout;
    auto* editGlobal = button(QStringLiteral("Editar synkro.yml"), QStringLiteral("edit"));
    connect(editGlobal, &QPushButton::clicked, this, [this] {
        YamlEditorDialog dlg(L"synkro.yml", this);
        if (dlg.exec() == QDialog::Accepted)
            AppContext::instance().reload();
    });
    auto* apply = button(QStringLiteral("Aplicar e agendar (administrador)"), QStringLiteral("shield"), true);
    apply->setEnabled(!set.hasErrors());
    connect(apply, &QPushButton::clicked, this, [this] {
        auto r = AppContext::instance().runWorkerElevated(
            {QStringLiteral("config"), QStringLiteral("apply"), QStringLiteral("--config-dir"), fmt::q(AppContext::instance().configDir())});
        if (!r) {
            QMessageBox::warning(this, QStringLiteral("Aplicar configuração"), fmt::q(r.error().message));
            return;
        }
        if (r.value() == 0)
            QMessageBox::information(this, QStringLiteral("Aplicar configuração"),
                                     QStringLiteral("Configuração aplicada e tarefas registradas no Agendador do Windows."));
        else
            QMessageBox::warning(this, QStringLiteral("Aplicar configuração"),
                                 QStringLiteral("O worker terminou com código %1. Rode \"synkro-worker config apply\" num "
                                                "prompt de administrador para ver os detalhes.").arg(r.value()));
        AppContext::instance().reload();
    });
    auto* openDir = button(QStringLiteral("Abrir pasta"), QStringLiteral("folder"));
    connect(openDir, &QPushButton::clicked, [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(fmt::q(AppContext::instance().configDir())));
    });
    auto* exportBtn = button(QStringLiteral("Exportar…"), QStringLiteral("external"));
    exportBtn->setEnabled(!set.hasErrors());
    exportBtn->setToolTip(QStringLiteral("Gera um pacote para importar em outro PDV (sem senhas e sem a identidade dos discos)"));
    connect(exportBtn, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getSaveFileName(this, QStringLiteral("Exportar configuração"),
                                                          QStringLiteral("synkro-config.yml"),
                                                          QStringLiteral("Pacote do Synkro (*.yml)"));
        if (file.isEmpty())
            return;
        auto pkg = synkro::config::exportPackage(AppContext::instance().config());
        if (!pkg) {
            QMessageBox::warning(this, QStringLiteral("Exportar configuração"), fmt::q(pkg.error().message));
            return;
        }
        if (auto st = synkro::win::writeFileAtomically(QDir::toNativeSeparators(file).toStdWString(), pkg.value()); !st) {
            QMessageBox::warning(this, QStringLiteral("Exportar configuração"), fmt::q(st.error().message));
            return;
        }
        QMessageBox::information(
            this, QStringLiteral("Exportar configuração"),
            QStringLiteral("Pacote gravado. No outro PDV, prepare os discos e importe como administrador:\n\n"
                           "synkro-worker config import --from \"%1\" --bind <cofre>=<disco>")
                .arg(QDir::toNativeSeparators(file)));
    });
    auto* importBtn = button(QStringLiteral("Importar…"), QStringLiteral("restore"));
    importBtn->setToolTip(QStringLiteral("Traz a configuração exportada de outro PDV"));
    connect(importBtn, &QPushButton::clicked, this, [this] {
        ImportConfigDialog dlg(this);
        dlg.exec();
    });
    // Relatório para o suporte (piloto): roda o worker como administrador para
    // conferir também segredos e Agendador.
    auto* diagBtn = button(QStringLiteral("Gerar diagnóstico…"), QStringLiteral("alert"));
    diagBtn->setToolTip(QStringLiteral("Relatório para o suporte: checagem da instalação, execuções, alertas e configuração "
                                       "(sem senhas)"));
    connect(diagBtn, &QPushButton::clicked, this, [this] {
        const QString file = QFileDialog::getSaveFileName(
            this, QStringLiteral("Salvar diagnóstico"),
            QDir(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation))
                .filePath(QStringLiteral("synkro-diagnostico-%1-%2.txt")
                              .arg(QSysInfo::machineHostName(),
                                   QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm")))),
            QStringLiteral("Texto (*.txt)"));
        if (file.isEmpty())
            return;
        auto r = AppContext::instance().runWorkerElevated({QStringLiteral("diag"), QStringLiteral("--to"),
                                                           QDir::toNativeSeparators(file), QStringLiteral("--config-dir"),
                                                           fmt::q(AppContext::instance().configDir())});
        if (!r || !QFileInfo::exists(file)) {
            QMessageBox::warning(this, QStringLiteral("Diagnóstico"),
                                 r ? QStringLiteral("O relatório não foi gerado (código %1).").arg(r.value())
                                   : fmt::q(r.error().message));
            return;
        }
        QMessageBox::information(this, QStringLiteral("Diagnóstico"),
                                 QStringLiteral("Relatório salvo em:\n%1\n\nEle não contém senhas, mas tem nomes de servidores, "
                                                "pastas e e-mails da configuração. Envie só ao suporte.")
                                     .arg(QDir::toNativeSeparators(file)));
        QDesktopServices::openUrl(QUrl::fromLocalFile(file));
    });
    crow->addWidget(editGlobal);
    crow->addWidget(openDir);
    crow->addWidget(exportBtn);
    crow->addWidget(importBtn);
    crow->addWidget(diagBtn);
    crow->addStretch(1);
    crow->addWidget(apply);
    cv->addLayout(crow);
    body()->addWidget(cfg);

    body()->addWidget(label(QStringLiteral("Este processo %1 com privilégio de administrador.")
                                .arg(synkro::win::isProcessElevated() ? QStringLiteral("está") : QStringLiteral("não está")),
                            "muted"));
}

void SettingsPage::refreshSecrets()
{
    if (!m_secrets)
        return;
    m_secrets->clear();
    auto names = synkro::SecretStore(synkro::secretsFile()).names();
    if (names) {
        for (const auto& n : names.value())
            m_secrets->addItem(fmt::q(n));
    }
}
