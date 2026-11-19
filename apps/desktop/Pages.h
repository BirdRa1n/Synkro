#pragma once

#include "Ui.h"

#include <QComboBox>
#include <QListWidget>
#include <QTableWidget>
#include <QTreeWidget>

#include <string>

class QGridLayout;

// Telas do Synkro Desktop. Cada uma reconstrói o conteúdo em refresh(), que é
// chamado ao abrir a tela, a cada recarga dos dados e quando o tema muda.

class DashboardPage : public ui::Page {
    Q_OBJECT
public:
    DashboardPage();
    void refresh() override;
signals:
    void runJobRequested(const QString& jobId);
};

class VaultsPage : public ui::Page {
    Q_OBJECT
public:
    VaultsPage();
    void refresh() override;
};

class JobsPage : public ui::Page {
    Q_OBJECT
public:
    JobsPage();
    void refresh() override;
signals:
    void runJobRequested(const QString& jobId);

private:
    QString selectedJob() const;
    void editSelected();
    void showPlan();
    QTreeWidget* m_tree = nullptr;
};

class HistoryPage : public ui::Page {
    Q_OBJECT
public:
    HistoryPage();
    void refresh() override;

private:
    QTableWidget* m_table = nullptr;
};

class RestorePage : public ui::Page {
    Q_OBJECT
public:
    RestorePage();
    void refresh() override;

private:
    void loadJobs();
    void loadPoints();
    void restore();

    QComboBox* m_vault = nullptr;
    QComboBox* m_job = nullptr;
    QTableWidget* m_points = nullptr;
    QLabel* m_hint = nullptr;
    QWidget* m_filesTarget = nullptr;
    QWidget* m_dbTarget = nullptr;
    class QLineEdit* m_targetFolder = nullptr;
    class QLineEdit* m_targetDb = nullptr;
    class QLineEdit* m_pathFilter = nullptr;
    bool m_isDatabase = false;
};

class SettingsPage : public ui::Page {
    Q_OBJECT
public:
    SettingsPage();
    void refresh() override;

private:
    void refreshSecrets();
    QListWidget* m_secrets = nullptr;
};
