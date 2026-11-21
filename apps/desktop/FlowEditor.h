#pragma once

// Modo fácil: a tarefa como um fluxo de blocos ligados (gatilhos → comandos →
// origem → cofres / comandos depois). Puxe blocos da paleta, ligue arrastando
// da bolinha de saída até outro bloco e ajuste cada bloco no painel à direita.
// O resultado é o mesmo YAML que o editor de texto gravaria
// (src/config/FlowGraph.h faz a conversão).

#include "config/Config.h"
#include "config/FlowGraph.h"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QStackedWidget;
class QTimeEdit;
class QTimer;

class FlowScene;
class FlowView;

class FlowEditorDialog : public QDialog {
    Q_OBJECT
public:
    explicit FlowEditorDialog(const synkro::config::JobConfig& job, QWidget* parent = nullptr);

    // Para capturas de tela: seleciona um bloco do tipo pedido.
    void selectFirst(synkro::config::flow::NodeKind kind);

private:
    void buildInspector();
    void showInspector(int nodeId);
    void loadTrigger(const synkro::config::flow::Node& n);
    void loadCommand(const synkro::config::flow::Node& n);
    void updateCommandRole(const synkro::config::flow::Node& n);
    void loadSource();
    void loadVault(const synkro::config::flow::Node& n);
    void storeTrigger();
    void storeCommand();
    void storeSource();
    void addBlock(const QString& type, QPointF scenePos);
    void changed();
    void validate();
    void save();
    void openYaml();

    synkro::config::JobConfig m_base;
    synkro::config::flow::Graph m_graph;
    FlowScene* m_scene;
    FlowView* m_view;
    int m_current = 0;
    bool m_loading = false;

    QStackedWidget* m_inspector;
    // gatilho
    QComboBox* m_trKind;
    QFormLayout* m_trForm;
    QTimeEdit* m_trTime;
    QWidget* m_trDaysBox;
    QCheckBox* m_trDays[7];
    QSpinBox *m_trDayOfMonth, *m_trInterval, *m_trDelay;
    // comando
    QLineEdit* m_cmdLine;
    QSpinBox* m_cmdTimeout;
    QComboBox* m_cmdOnError;
    QLabel* m_cmdRole;
    // origem
    QLineEdit *m_srcName, *m_srcPath, *m_srcExclude;
    QWidget* m_srcFilesBox;
    QLabel* m_srcDbInfo;
    QCheckBox* m_srcShutdown;
    // cofre
    QLabel* m_vaultInfo;

    QLabel* m_status;
    QPushButton* m_save;
    QTimer* m_debounce;
    bool m_hasErrors = false;
};
