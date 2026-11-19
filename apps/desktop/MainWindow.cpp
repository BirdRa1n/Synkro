#include "MainWindow.h"

#include "AppContext.h"
#include "Dialogs.h"
#include "Format.h"
#include "Icons.h"
#include "Pages.h"
#include "Theme.h"

#include "core/Version.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCloseEvent>
#include <QDir>
#include <QHBoxLayout>
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
    setWindowIcon(icons::trayIcon(QColor("#4338CA")));
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
        menu->addAction(QStringLiteral("Sair"), qApp, &QApplication::quit);
        m_tray->setContextMenu(menu);
        connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
            if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) {
                showNormal();
                activateWindow();
            }
        });
        m_tray->show();
    }

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
        logo->setPixmap(icons::pixmap(QStringLiteral("shield-check"), Qt::white, 30));
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
    if (m_tray && m_tray->isVisible() && !property("quitting").toBool()) {
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
        shoot(&wizard, QStringLiteral("7-assistente-origem.png"));
    }
    if (!AppContext::instance().config().jobs.empty()) {
        YamlEditorDialog editor(AppContext::instance().config().jobs.front().file, this);
        shoot(&editor, QStringLiteral("8-editor-yaml.png"));
    }
    {
        AddVaultDialog add(this);
        shoot(&add, QStringLiteral("9-adicionar-disco.png"));
    }
    {
        ImportConfigDialog import(this);
        shoot(&import, QStringLiteral("10-importar-config.png"));
    }
    Theme::instance().setMode(original); // não altera a preferência do usuário
}
