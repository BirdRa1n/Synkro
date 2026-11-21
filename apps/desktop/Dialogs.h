#pragma once

#include "config/Config.h"

#include <QDialog>
#include <QProcess>
#include <QWizard>

#include <map>
#include <string>
#include <utility>
#include <vector>

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

// Adicionar um cofre: disco desta máquina ou pasta na rede (NAS/servidor).
// Escolha, checagem (architecture.md §3.1 e §3.3.1), inicialização e registro
// em synkro.yml; em rede, a senha vai para o cofre de segredos.
class AddVaultDialog : public QDialog {
    Q_OBJECT
public:
    explicit AddVaultDialog(QWidget* parent = nullptr, bool network = false);

private:
    void runCheck();
    void initialize();
    bool network() const;
    void updateKind();
    QComboBox* m_kind;
    QWidget* m_diskGroup;
    QWidget* m_netGroup;
    QLineEdit* m_unc;
    QLineEdit* m_netUser;
    QLineEdit* m_netPassword;
    QLabel* m_intro;
    class QFormLayout* m_nameForm;
    QComboBox* m_drive;
    QLineEdit* m_label;
    QCheckBox* m_rotation;
    QCheckBox* m_acceptWarnings;
    QVBoxLayout* m_findings;
    class QPushButton* m_initButton;
    bool m_accepted = false;
    bool m_hasWarnings = false;
    std::wstring m_mountPoint;
    std::wstring m_checkedPath; // caminho examinado na última verificação
};

// Abrir um cofre numa pasta de rede para restaurar (inclusive de um
// computador novo, sem configuração): caminho, usuário e senha.
class OpenNetworkVaultDialog : public QDialog {
    Q_OBJECT
public:
    explicit OpenNetworkVaultDialog(QWidget* parent = nullptr);
    std::wstring location() const { return m_location; }
    QString vaultLabel() const { return m_vaultLabel; }

private:
    void open();
    QLineEdit* m_unc;
    QLineEdit* m_user;
    QLineEdit* m_password;
    QLabel* m_status;
    class QPushButton* m_openButton;
    std::wstring m_location;
    QString m_vaultLabel;
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

// Importar um pacote de configuração de outro PDV (configuration.md §12):
// liga cada cofre do pacote a um disco desta máquina, simula sem
// administrador e importa pelo worker elevado.
class ImportConfigDialog : public QDialog {
    Q_OBJECT
public:
    explicit ImportConfigDialog(QWidget* parent = nullptr);

private:
    void choosePackage();
    bool simulate();
    void runImport();
    std::map<std::string, std::wstring> bindings() const;

    QString m_file;
    QLabel* m_source;
    QVBoxLayout* m_vaultRows;
    std::vector<std::pair<std::string, QComboBox*>> m_vaultCombos;
    QCheckBox* m_replace;
    QTextBrowser* m_report;
    class QPushButton* m_import;
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
