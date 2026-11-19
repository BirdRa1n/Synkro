#pragma once

#include "config/Config.h"

#include <QDialog>
#include <QProcess>
#include <QWizard>

#include <string>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;
class QTextBrowser;
class QTimeEdit;
class QTimer;
class QVBoxLayout;

// Adicionar um disco como cofre: escolha, checagem (architecture.md §3.1),
// inicialização e registro em synkro.yml.
class AddVaultDialog : public QDialog {
    Q_OBJECT
public:
    explicit AddVaultDialog(QWidget* parent = nullptr);

private:
    void runCheck();
    void initialize();
    QComboBox* m_drive;
    QLineEdit* m_label;
    QCheckBox* m_rotation;
    QCheckBox* m_acceptWarnings;
    QVBoxLayout* m_findings;
    class QPushButton* m_initButton;
    bool m_accepted = false;
    bool m_hasWarnings = false;
    std::wstring m_mountPoint;
};

// Assistente de criação de tarefa (arquivos ou banco de dados).
class JobWizard : public QWizard {
    Q_OBJECT
public:
    explicit JobWizard(QWidget* parent = nullptr);
    void accept() override;

private:
    synkro::config::JobConfig buildJob() const;
    void testConnection();
    void updateReview();

    QLineEdit *m_name, *m_source, *m_exclude, *m_sentinel;
    QComboBox *m_type, *m_engine;
    QLineEdit *m_host, *m_user, *m_password, *m_secretName, *m_databases, *m_sqliteFiles;
    QSpinBox* m_port;
    QLabel* m_connectionResult;
    QListWidget* m_vaults;
    QCheckBox* m_days[7];
    QTimeEdit* m_time;
    QCheckBox *m_onStartup, *m_shutdown;
    QTextBrowser* m_review;
    int m_filesPage = 0, m_dbPage = 0;
};

// Editor YAML com validação ao digitar e o painel "O que esta tarefa vai fazer".
class YamlEditorDialog : public QDialog {
    Q_OBJECT
public:
    // `relativePath`: "synkro.yml" ou "jobs\\<id>.yml".
    explicit YamlEditorDialog(const std::wstring& relativePath, QWidget* parent = nullptr);

private:
    void validate();
    void save();
    std::wstring m_path;
    bool m_isGlobal;
    QPlainTextEdit* m_editor;
    QListWidget* m_diagnostics;
    QTextBrowser* m_plan;
    class QPushButton* m_save;
    QTimer* m_debounce;
    bool m_hasErrors = false;
};

// Cadastro de segredo (valor nunca exibido).
class SecretDialog : public QDialog {
    Q_OBJECT
public:
    explicit SecretDialog(const QString& name, QWidget* parent = nullptr);
    void accept() override;
    QString name() const;

private:
    QLineEdit* m_name;
    QLineEdit* m_value;
    QLineEdit* m_confirm;
};

// Execução de uma tarefa com a saída do worker ao vivo.
class RunDialog : public QDialog {
    Q_OBJECT
public:
    RunDialog(const QString& jobId, QWidget* parent = nullptr);

private:
    QProcess m_process;
    QPlainTextEdit* m_log;
    QLabel* m_status;
    class QPushButton* m_close;
};
