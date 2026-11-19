#pragma once

#include <QMainWindow>
#include <QSet>
#include <QSystemTrayIcon>

#include <vector>

class QButtonGroup;
class QStackedWidget;
class QToolButton;
namespace ui {
class Page;
}

// Janela principal: trilho de navegação à esquerda (proposta de design 5),
// telas empilhadas e ícone na bandeja com o estado dos backups.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();

    int pageCount() const;
    void showPage(int index);
    QString pageName(int index) const;

    // Grava uma captura de cada tela em `dir` (claro e escuro) — usado no
    // desenvolvimento para revisar o visual.
    void saveScreenshots(const QString& dir);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void buildNav();
    void refreshNavIcons();
    void refreshCurrent();
    void updateTray();
    void runJob(const QString& jobId);

    QStackedWidget* m_stack = nullptr;
    std::vector<ui::Page*> m_pages;
    std::vector<QToolButton*> m_navButtons;
    std::vector<QToolButton*> m_themeButtons;
    QSystemTrayIcon* m_tray = nullptr;
    QSet<QString> m_knownAlerts;
    bool m_trayNoticeShown = false;
};
