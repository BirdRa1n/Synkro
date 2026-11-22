#pragma once

// Peças de edição de tarefa usadas pelo assistente e pelo modo fácil.

#include "config/Config.h"

#include <QWidget>

class QCheckBox;
class QLabel;
class QTableWidget;
class QTimeEdit;

// Lista de pastas de origem. Uma pasta só: fica na raiz do cofre. Várias:
// cada uma ganha uma subpasta no cofre, com o nome da própria pasta ou outro
// informado na coluna "No cofre".
class SourceListEditor : public QWidget {
    Q_OBJECT
public:
    explicit SourceListEditor(QWidget* parent = nullptr);

    void setSources(const synkro::config::FilesJob& files);
    void applyTo(synkro::config::FilesJob& files) const; // source, sourceName/Alias e moreSources
    bool isEmpty() const;

signals:
    void changed();

private:
    void addRow(const QString& path, const QString& name);
    void addFolder();
    void renumber();

    QTableWidget* m_table;
    bool m_loading = false;
};

// Dias da semana com horário: o mesmo para todos ou um horário próprio por dia
// (ex.: domingo ao meio-dia). Vira um gatilho por horário distinto.
class WeekScheduleEditor : public QWidget {
    Q_OBJECT
public:
    explicit WeekScheduleEditor(QWidget* parent = nullptr);

    std::vector<synkro::config::ScheduleTrigger> triggers() const;

private:
    void updateMode();

    QCheckBox* m_sameTime;
    QTimeEdit* m_time;
    QCheckBox* m_days[7];
    QTimeEdit* m_dayTimes[7];
};

// Agrupa (dia → horário) em gatilhos: todos os 7 dias no mesmo horário viram
// `daily`; os demais, um `weekly` por horário.
std::vector<synkro::config::ScheduleTrigger> triggersFromDayTimes(const std::vector<std::pair<int, int>>& dayMinutes);
