#include "MainWindow.h"

#include "AppContext.h"
#include "Dialogs.h"
#include "FlowEditor.h"
#include "JobWidgets.h"
#include "Format.h"
#include "Icons.h"
#include "Pages.h"
#include "Theme.h"
#include "Updater.h"

#include "config/ConfigWriter.h"
#include "core/Version.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QTimeEdit>
#include <QCloseEvent>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSessionManager>
#include <QMenu>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

struct NavEntry {
    const char* label;
    const char* icon;
};

const NavEntry kNav[] = {
    {"Painel", "home"}, {"Tarefas", "jobs"}, {"Cofres", "disk"}, {"Histórico", "history"}, {"Restaurar", "restore"},
    {"Config", "settings"},
};

} // namespace

MainWindow::MainWindow()
{
    setWindowTitle(QStringLiteral("Synkro"));
    setWindowIcon(icons::appIcon());
    resize(1320, 880);
    setMinimumSize(960, 640);

    auto* central = new QWidget;
    auto* h = new QHBoxLayout(central);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);

    auto* rail = new QWidget;
    rail->setObjectName("navRail");
    rail->setFixedWidth(92);
    rail->setAttribute(Qt::WA_StyledBackground);
    auto* rv = new QVBoxLayout(rail);
    rv->setContentsMargins(12, 20, 12, 16);
    rv->setSpacing(8);
    auto* logo = new QLabel;
    logo->setObjectName("logo");
    logo->setAlignment(Qt::AlignCenter);
    rv->addWidget(logo);
    rv->addSpacing(14);

    auto* group = new QButtonGroup(this);
    for (int i = 0; i < static_cast<int>(std::size(kNav)); ++i) {
        auto* b = new QToolButton;
        b->setObjectName("navButton");
        b->setText(QString::fromUtf8(kNav[i].label));
        b->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        b->setCheckable(true);
        b->setFixedSize(68, 60);
        b->setIconSize(QSize(22, 22));
        b->setCursor(Qt::PointingHandCursor);
        group->addButton(b, i);
        rv->addWidget(b, 0, Qt::AlignHCenter);
        m_navButtons.push_back(b);
    }
    connect(group, &QButtonGroup::idClicked, this, [this](int id) { showPage(id); });
    rv->addStretch(1);

    // Tema: claro / escuro / sistema.
    auto* themeGroup = new QButtonGroup(this);
    const std::pair<ThemeMode, const char*> modes[] = {{ThemeMode::Light, "sun"}, {ThemeMode::Dark, "moon"}, {ThemeMode::System, "monitor"}};
    for (const auto& [mode, icon] : modes) {
        auto* b = new QToolButton;
        b->setObjectName("themeButton");
        b->setCheckable(true);
        b->setToolTip(QStringLiteral("Tema: ") + Theme::modeName(mode));
        b->setChecked(Theme::instance().mode() == mode);
        b->setFixedSize(34, 30);
        b->setProperty("iconName", QString::fromLatin1(icon));
        themeGroup->addButton(b);
        const ThemeMode m = mode;
        connect(b, &QToolButton::clicked, [m] { Theme::instance().setMode(m); });
        rv->addWidget(b, 0, Qt::AlignHCenter);
        m_themeButtons.push_back(b);
    }
    auto* version = new QLabel(QString::fromLatin1(synkro::versionString()));
    version->setAlignment(Qt::AlignCenter);
    version->setStyleSheet(QStringLiteral("color:#64748B; font-size:8pt;"));
    rv->addWidget(version);
    h->addWidget(rail);

    m_stack = new QStackedWidget;
    auto* dashboard = new DashboardPage;
    auto* jobs = new JobsPage;
    m_pages = {dashboard, jobs, new VaultsPage, new HistoryPage, new RestorePage, new SettingsPage};
    for (auto* p : m_pages)
        m_stack->addWidget(p);
    connect(dashboard, &DashboardPage::runJobRequested, this, &MainWindow::runJob);
    connect(jobs, &JobsPage::runJobRequested, this, &MainWindow::runJob);
    h->addWidget(m_stack, 1);
    setCentralWidget(central);

    // Bandeja.
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        m_tray = new QSystemTrayIcon(this);
        auto* menu = new QMenu(this);
        menu->addAction(QStringLiteral("Abrir o Synkro"), this, [this] {
            showNormal();
            raise();
            activateWindow();
        });
        menu->addAction(QStringLiteral("Situação dos backups"), this, [this] {
            showPage(0);
            showNormal();
            activateWindow();
        });
        menu->addSeparator();
        menu->addAction(QStringLiteral("Sair"), this, [this] {
            setProperty("quitting", true); // senão o closeEvent recusa e o app só volta para a bandeja
            qApp->quit();
        });
        m_tray->setContextMenu(menu);
        connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
            if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) {
                showNormal();
                activateWindow();
            }
        });
        m_tray->show();
    }

    // Windows desligando ou saindo da sessão: o Qt fecha as janelas visíveis e,
    // se uma recusa (o closeEvent abaixo só esconde na bandeja), CANCELA o
    // desligamento — o computador ficava ligado depois do backup com a janela
    // do Synkro aberta. Na saída da sessão, fechar de verdade.
    connect(qApp, &QGuiApplication::commitDataRequest, this, [this](QSessionManager&) { setProperty("quitting", true); });

    connect(&Theme::instance(), &Theme::changed, this, [this] {
        const ThemeMode order[] = {ThemeMode::Light, ThemeMode::Dark, ThemeMode::System};
        for (size_t i = 0; i < m_themeButtons.size(); ++i)
            m_themeButtons[i]->setChecked(Theme::instance().mode() == order[i]);
        refreshNavIcons();
        ui::retintButtons(this);
        refreshCurrent();
    });
    connect(&AppContext::instance(), &AppContext::reloaded, this, [this] {
        refreshCurrent();
        updateTray();
    });
    auto* timer = new QTimer(this);
    timer->setInterval(60 * 1000);
    connect(timer, &QTimer::timeout, [] { AppContext::instance().reload(); });
    timer->start();

    // Atualização automática: baixada em segundo plano; a bandeja avisa quando
    // está pronta e um clique no aviso leva à instalação.
    connect(&Updater::instance(), &Updater::readyToInstall, this, [this](const QString& newVersion) {
        if (m_tray)
            m_tray->showMessage(QStringLiteral("Atualização do Synkro"),
                                QStringLiteral("A versão %1 foi baixada. Clique para instalar.").arg(newVersion),
                                QSystemTrayIcon::Information, 15000);
    });
    if (m_tray)
        connect(m_tray, &QSystemTrayIcon::messageClicked, this, [this] {
            if (Updater::instance().state() != Updater::State::Ready)
                return;
            showNormal();
            activateWindow();
            SettingsPage::confirmInstall(this);
        });
    Updater::instance().start();

    refreshNavIcons();
    showPage(0);
    updateTray();
}

int MainWindow::pageCount() const
{
    return static_cast<int>(m_pages.size());
}

QString MainWindow::pageName(int index) const
{
    return QString::fromUtf8(kNav[index].label);
}

void MainWindow::showPage(int index)
{
    if (index < 0 || index >= pageCount())
        return;
    m_navButtons[static_cast<size_t>(index)]->setChecked(true);
    m_stack->setCurrentIndex(index);
    m_pages[static_cast<size_t>(index)]->refresh();
}

void MainWindow::refreshCurrent()
{
    if (auto* p = static_cast<ui::Page*>(m_stack->currentWidget()))
        p->refresh();
}

void MainWindow::refreshNavIcons()
{
    const auto& t = tk();
    for (size_t i = 0; i < m_navButtons.size(); ++i)
        m_navButtons[i]->setIcon(icons::icon(QString::fromLatin1(kNav[i].icon), m_navButtons[i]->isChecked() ? QColor(Qt::white) : t.navInk));
    for (auto* b : m_themeButtons)
        b->setIcon(icons::icon(b->property("iconName").toString(), b->isChecked() ? QColor(Qt::white) : t.navInk, 16));
    if (auto* logo = findChild<QLabel*>(QStringLiteral("logo")))
        logo->setPixmap(icons::appPixmap(44));
}

void MainWindow::updateTray()
{
    if (!m_tray)
        return;
    const auto& ctx = AppContext::instance();
    const int level = ctx.severityLevel();
    const QColor color = level == 2 ? QColor("#DC2626") : level == 1 ? QColor("#D97706") : QColor("#16A34A");
    m_tray->setIcon(icons::trayIcon(color));
    QString tip = level == 0 ? QStringLiteral("Synkro: backups em dia")
                             : QStringLiteral("Synkro: %1 alerta(s)").arg(ctx.alerts().size());
    m_tray->setToolTip(tip);

    // Notifica alertas novos (uma vez cada).
    QSet<QString> current;
    for (const auto& a : ctx.alerts()) {
        const QString key = fmt::q(a.code + "/" + a.subject);
        current.insert(key);
        if (!m_knownAlerts.contains(key) && ctx.config().global.alertTray)
            m_tray->showMessage(QStringLiteral("Synkro"), fmt::q(a.message),
                                a.severity == synkro::AlertSeverity::Critical ? QSystemTrayIcon::Critical
                                                                              : QSystemTrayIcon::Warning);
    }
    m_knownAlerts = current;
    refreshNavIcons();
}

void MainWindow::runJob(const QString& jobId)
{
    RunDialog dlg(jobId, this);
    dlg.exec();
    AppContext::instance().reload();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // Fechar a janela mantém o Synkro na bandeja, de olho nos alertas.
    if (m_tray && m_tray->isVisible() && !property("quitting").toBool() && !qApp->isSavingSession()) {
        hide();
        if (!m_trayNoticeShown) {
            m_tray->showMessage(QStringLiteral("Synkro"),
                                QStringLiteral("O Synkro continua na bandeja acompanhando os backups. Para sair, use o menu do ícone."));
            m_trayNoticeShown = true;
        }
        event->ignore();
        return;
    }
    QMainWindow::closeEvent(event);
}

void MainWindow::saveScreenshots(const QString& dir)
{
    QDir().mkpath(dir);
    const ThemeMode original = Theme::instance().mode();
    const QStringList files = {QStringLiteral("1-painel"), QStringLiteral("2-tarefas"), QStringLiteral("3-cofres"),
                               QStringLiteral("4-historico"), QStringLiteral("5-restaurar"), QStringLiteral("6-config")};
    for (ThemeMode mode : {ThemeMode::Light, ThemeMode::Dark}) {
        Theme::instance().setMode(mode);
        for (int i = 0; i < pageCount(); ++i) {
            showPage(i);
            for (int k = 0; k < 5; ++k)
                QApplication::processEvents();
            grab().save(QDir(dir).filePath(files[i] + (mode == ThemeMode::Dark ? QStringLiteral("-escuro.png")
                                                                               : QStringLiteral("-claro.png"))));
        }
    }
    // Diálogos principais (tema claro).
    Theme::instance().setMode(ThemeMode::Light);
    auto shoot = [&](QWidget* w, const QString& name) {
        w->show();
        for (int k = 0; k < 5; ++k)
            QApplication::processEvents();
        w->grab().save(QDir(dir).filePath(name));
        w->close();
    };
    {
        JobWizard wizard(this);
        shoot(&wizard, QStringLiteral("7-assistente-inicio.png"));
        wizard.show();
        wizard.next();
        if (auto* list = wizard.findChild<SourceListEditor*>()) {
            synkro::config::FilesJob demo;
            demo.source = L"C:\\PDV";
            demo.sourceAlias = L"PDV";
            demo.moreSources = {{L"D:\\Fiscal\\XML", {}, L"XML"}, {L"E:\\Exportacao", L"exportacao-erp", L"exportacao-erp"}};
            list->setSources(demo);
        }
        shoot(&wizard, QStringLiteral("7-assistente-origem.png"));
        wizard.show(); // ao reabrir, o assistente volta ao início
        wizard.next();
        wizard.next();
        for (auto* cb : wizard.findChildren<QCheckBox*>())
            if (cb->text().startsWith(QStringLiteral("Mesmo horário")))
                cb->setChecked(false);
            else if (cb->text() == QLatin1String("Dom"))
                cb->setChecked(true);
        if (const auto times = wizard.findChildren<QTimeEdit*>(); !times.isEmpty())
            times.constLast()->setTime(QTime(12, 0)); // domingo ao meio-dia
        shoot(&wizard, QStringLiteral("7-assistente-quando.png"));
    }
    if (!AppContext::instance().config().jobs.empty()) {
        YamlEditorDialog editor(AppContext::instance().config().jobs.front().file, this);
        shoot(&editor, QStringLiteral("8-editor-yaml.png"));
    }
    if (!AppContext::instance().config().jobs.empty()) {
        // Modo fácil: um exemplo com comandos antes/depois, além da tarefa real.
        auto job = AppContext::instance().config().jobs.front();
        for (ThemeMode mode : {ThemeMode::Light, ThemeMode::Dark}) {
            Theme::instance().setMode(mode);
            const QString suffix = mode == ThemeMode::Dark ? QStringLiteral("-escuro.png") : QStringLiteral("-claro.png");
            FlowEditorDialog flow(job, this);
            flow.selectFirst(synkro::config::flow::NodeKind::Source);
            shoot(&flow, QStringLiteral("15-modo-facil") + suffix);
        }
        Theme::instance().setMode(ThemeMode::Light);
        synkro::config::HookCommand before, after;
        before.run = {"C:\\PDV\\parar-servico.cmd"};
        after.run = {"C:\\PDV\\iniciar-servico.cmd"};
        job.hooks.before = {before};
        job.hooks.onSuccess = {after};
        synkro::config::ScheduleTrigger boot;
        boot.kind = synkro::config::TriggerKind::OnStartup;
        boot.delaySeconds = 600;
        job.schedule.push_back(boot);
        FlowEditorDialog flow(job, this);
        flow.selectFirst(synkro::config::flow::NodeKind::Command);
        shoot(&flow, QStringLiteral("15-modo-facil-comandos.png"));
        for (const auto& j : AppContext::instance().config().jobs)
            if (job.type != "files" && j.type == "files")
                job = j;
        if (job.type == "files") {
            job.files.moreSources.push_back({L"D:\\Fiscal\\XML", {}, L"XML"});
            job.files.moreSources.push_back({L"E:\\Exportacao", L"exportacao-erp", L"exportacao-erp"});
            FlowEditorDialog many(job, this);
            many.selectFirst(synkro::config::flow::NodeKind::Source);
            shoot(&many, QStringLiteral("15-modo-facil-pastas.png"));
        }
    }
    {
        AddVaultDialog add(this);
        shoot(&add, QStringLiteral("9-adicionar-disco.png"));
    }
    if (const QString textFile = qEnvironmentVariable("SYNKRO_SCREENSHOT_EDITOR_TEXT"); !textFile.isEmpty()) {
        // Desenvolvimento: editor do synkro.yml com um texto proposto (ex.: reproduzir um caso de suporte).
        QFile f(textFile);
        if (f.open(QIODevice::ReadOnly)) {
            YamlEditorDialog editor(L"synkro.yml", this, QString::fromUtf8(f.readAll()));
            shoot(&editor, QStringLiteral("8-editor-yaml-texto.png"));
        }
    }
    {
        // "Revisar no editor YAML" depois de adicionar um cofre (exemplo).
        synkro::config::VaultConfig sample;
        sample.id = "hd-externo-2";
        sample.vaultId = "9d3f6a10-5e2b-4c8d-b7a1-3f0e9c2d4b55";
        sample.label = "HD Externo 2";
        sample.path = L"F:\\";
        sample.rotation = true;
        auto proposed = synkro::config::insertVaultIntoGlobalYaml(AppContext::instance().readConfigFile(L"synkro.yml"), sample);
        if (proposed) {
            YamlEditorDialog editor(L"synkro.yml", this, QString::fromStdString(proposed.value()),
                                    QStringLiteral("O cofre \"HD Externo 2\" (id hd-externo-2) foi inserido na lista vaults. "
                                                   "Confira e clique em Salvar; Cancelar não altera o arquivo."));
            shoot(&editor, QStringLiteral("8-editor-yaml-cofre-novo.png"));
        }
    }
    {
        AddVaultDialog add(this, /*network=*/true);
        shoot(&add, QStringLiteral("9-adicionar-rede.png"));
        // Verificação de verdade, com a pasta temporária pelo compartilhamento
        // administrativo desta máquina (deve ser recusada: é a própria máquina).
        const QString temp = QDir::toNativeSeparators(QDir::tempPath());
        const QString unc = QStringLiteral("\\\\localhost\\") + temp.left(1) + QStringLiteral("$") + temp.mid(2);
        const auto edits = add.findChildren<QLineEdit*>();
        if (!edits.isEmpty()) {
            edits.front()->setText(unc);
            add.show();
            for (auto* b : add.findChildren<QPushButton*>())
                if (b->text() == QStringLiteral("Verificar"))
                    b->click();
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < 20000) { // espera a checagem em segundo plano
                QApplication::processEvents(QEventLoop::AllEvents, 100);
                bool done = false;
                for (auto* l : add.findChildren<QLabel*>())
                    done = done || l->text().contains(QStringLiteral("não pode ser usada"))
                           || l->text().contains(QStringLiteral("Teste de gravação"));
                if (done)
                    break;
            }
            shoot(&add, QStringLiteral("9-adicionar-rede-verificado.png"));
        }
    }
    {
        OpenNetworkVaultDialog open(this);
        shoot(&open, QStringLiteral("11-abrir-cofre-rede.png"));
        // Pasta acessível, mas sem cofre: a mensagem precisa dizer o que procurar.
        const QString temp = QDir::toNativeSeparators(QDir::tempPath());
        const auto edits = open.findChildren<QLineEdit*>();
        if (!edits.isEmpty()) {
            edits.front()->setText(QStringLiteral("\\\\localhost\\") + temp.left(1) + QStringLiteral("$") + temp.mid(2));
            open.show();
            for (auto* b : open.findChildren<QPushButton*>())
                if (b->text() == QStringLiteral("Abrir"))
                    b->click();
            QElapsedTimer t;
            t.start();
            while (t.elapsed() < 20000) {
                QApplication::processEvents(QEventLoop::AllEvents, 100);
                bool done = false;
                for (auto* l : open.findChildren<QLabel*>())
                    done = done || l->text().contains(QStringLiteral("cofre do Synkro"));
                if (done)
                    break;
            }
            shoot(&open, QStringLiteral("11-abrir-cofre-rede-sem-cofre.png"));
        }
    }
    {
        // Chave de exemplo (não é de nenhum cofre).
        RecoveryKeyDialog key(QStringLiteral("HD Externo 1"), QStringLiteral("4TX2-WMS6-WP7P-NLLF-LAA3-YB77-3LNW-GDQB"), true,
                              QString(), this);
        shoot(&key, QStringLiteral("12-chave-recuperacao.png"));
    }
    if (!AppContext::instance().config().global.vaults.empty()) {
        RemoveVaultDialog remove(AppContext::instance().config().global.vaults.front().id, L"E:\\", this);
        shoot(&remove, QStringLiteral("14-remover-cofre.png"));
    }
    {
        ImportConfigDialog import(this);
        shoot(&import, QStringLiteral("10-importar-config.png"));
    }
    // Atualizador contra as releases de verdade (desenvolvimento): procura,
    // baixa, confere o SHA-256 e fotografa o card. Nunca instala.
    if (qEnvironmentVariableIntValue("SYNKRO_SCREENSHOT_UPDATE") == 1) {
        auto& up = Updater::instance();
        up.check(true);
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < 180000) {
            QApplication::processEvents(QEventLoop::AllEvents, 200);
            const auto s = up.state();
            if (s != Updater::State::Checking && s != Updater::State::Downloading
                && !(s == Updater::State::Available && up.autoDownload()))
                break;
        }
        showPage(pageCount() - 1); // Configurações
        for (int k = 0; k < 10; ++k)
            QApplication::processEvents();
        grab().save(QDir(dir).filePath(QStringLiteral("13-atualizacao.png")));
        QFile log(QDir(dir).filePath(QStringLiteral("13-atualizacao.txt")));
        if (log.open(QIODevice::WriteOnly | QIODevice::Text))
            log.write(QStringLiteral("estado=%1\nerro=%2\nversao=%3\narquivo=%4\n")
                          .arg(Updater::stateText(up.state()), up.errorText(), up.release().version, up.downloadedFile())
                          .toUtf8());
    }
    Theme::instance().setMode(original); // não altera a preferência do usuário
}
