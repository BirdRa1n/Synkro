#include "JobWidgets.h"

#include "Theme.h"
#include "Ui.h"

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTimeEdit>
#include <QVBoxLayout>

#include <map>

using namespace synkro::config;

namespace {

QString folderName(const QString& path)
{
    QString p = QDir::fromNativeSeparators(path.trimmed());
    while (p.size() > 3 && p.endsWith('/'))
        p.chop(1);
    const int cut = p.lastIndexOf('/');
    return cut < 0 ? QString() : p.mid(cut + 1);
}

} // namespace

// ============================================================================
// SourceListEditor
// ============================================================================

SourceListEditor::SourceListEditor(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(6);
    m_table = new QTableWidget(0, 2);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Pasta"), QStringLiteral("No cofre")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Interactive);
    m_table->setColumnWidth(1, 150);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setMinimumHeight(120);
    m_table->setMaximumHeight(200);
    v->addWidget(m_table);

    auto* row = new QHBoxLayout;
    auto* add = ui::button(QStringLiteral("Adicionar pasta…"), QStringLiteral("plus"));
    auto* remove = ui::button(QStringLiteral("Remover"), QStringLiteral("trash"));
    connect(add, &QPushButton::clicked, this, [this] { addFolder(); });
    connect(remove, &QPushButton::clicked, this, [this] {
        const int r = m_table->currentRow();
        if (r < 0)
            return;
        m_table->removeRow(r);
        renumber();
        emit changed();
    });
    row->addWidget(add);
    row->addWidget(remove);
    row->addStretch(1);
    v->addLayout(row);
    auto* hint = ui::label(QStringLiteral("Com uma pasta, os arquivos ficam na raiz do cofre. Com várias, cada pasta ganha "
                                          "uma subpasta com o nome da coluna \"No cofre\" (dá para editar). Os filtros "
                                          "valem para todas."),
                           "muted");
    hint->setWordWrap(true);
    v->addWidget(hint);

    connect(m_table, &QTableWidget::itemChanged, this, [this] {
        if (!m_loading)
            emit changed();
    });
}

void SourceListEditor::addRow(const QString& path, const QString& name)
{
    m_loading = true;
    const int r = m_table->rowCount();
    m_table->insertRow(r);
    auto* p = new QTableWidgetItem(QDir::toNativeSeparators(path));
    m_table->setItem(r, 0, p);
    auto* n = new QTableWidgetItem(name);
    m_table->setItem(r, 1, n);
    m_loading = false;
    renumber();
}

void SourceListEditor::renumber()
{
    m_loading = true;
    const bool single = m_table->rowCount() == 1;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        auto* n = m_table->item(r, 1);
        if (!n)
            continue;
        if (single) {
            // Guarda o nome escolhido (se houver) para quando voltar a ter várias.
            if (n->text() != QLatin1String("(raiz)"))
                n->setData(Qt::UserRole, n->text());
            n->setText(QStringLiteral("(raiz)"));
            n->setFlags(n->flags() & ~Qt::ItemIsEditable);
            n->setForeground(tk().muted);
        } else {
            n->setFlags(n->flags() | Qt::ItemIsEditable);
            n->setForeground(tk().text);
            if (n->text().isEmpty() || n->text() == QLatin1String("(raiz)")) {
                const QString kept = n->data(Qt::UserRole).toString();
                n->setText(kept.isEmpty() ? folderName(m_table->item(r, 0)->text()) : kept);
            }
        }
    }
    m_loading = false;
}

void SourceListEditor::addFolder()
{
    const QString start = m_table->rowCount() ? m_table->item(m_table->rowCount() - 1, 0)->text() : QString();
    const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("Pasta de origem"), start);
    if (d.isEmpty())
        return;
    addRow(d, QString());
    emit changed();
}

void SourceListEditor::setSources(const FilesJob& f)
{
    m_table->setRowCount(0);
    if (!f.source.empty())
        addRow(QString::fromStdWString(f.source), QString::fromStdWString(f.sourceName));
    for (const auto& s : f.moreSources)
        addRow(QString::fromStdWString(s.path), QString::fromStdWString(s.alias.empty() ? s.name : s.alias));
    renumber();
}

bool SourceListEditor::isEmpty() const
{
    return m_table->rowCount() == 0;
}

void SourceListEditor::applyTo(FilesJob& f) const
{
    f.source.clear();
    f.sourceName.clear();
    f.sourceAlias.clear();
    f.moreSources.clear();
    const bool single = m_table->rowCount() == 1;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        SourceFolder s;
        const QString path = m_table->item(r, 0)->text().trimmed();
        s.path = QDir::toNativeSeparators(path).toStdWString();
        QString name = m_table->item(r, 1) ? m_table->item(r, 1)->text().trimmed() : QString();
        if (single || name == QLatin1String("(raiz)"))
            name.clear();
        // Só grava `name` quando difere do nome da pasta (YAML mais limpo).
        if (!name.isEmpty() && name != folderName(path))
            s.name = name.toStdWString();
        s.alias = name.isEmpty() ? folderName(path).toStdWString() : name.toStdWString();
        if (r == 0) {
            f.source = s.path;
            f.sourceName = s.name;
            f.sourceAlias = s.alias;
        } else {
            f.moreSources.push_back(std::move(s));
        }
    }
}

// ============================================================================
// WeekScheduleEditor
// ============================================================================

std::vector<ScheduleTrigger> triggersFromDayTimes(const std::vector<std::pair<int, int>>& dayMinutes)
{
    std::map<int, std::vector<int>> byTime; // minutos → dias
    for (const auto& [day, minutes] : dayMinutes)
        byTime[minutes].push_back(day);
    std::vector<ScheduleTrigger> out;
    for (const auto& [minutes, days] : byTime) {
        ScheduleTrigger t;
        t.atMinutes = minutes;
        if (days.size() == 7) {
            t.kind = TriggerKind::Daily;
        } else {
            t.kind = TriggerKind::Weekly;
            t.days = days;
        }
        out.push_back(t);
    }
    return out;
}

WeekScheduleEditor::WeekScheduleEditor(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(6);
    auto* top = new QHBoxLayout;
    m_sameTime = new QCheckBox(QStringLiteral("Mesmo horário em todos os dias, às"));
    m_sameTime->setChecked(true);
    m_time = new QTimeEdit(QTime(22, 0));
    m_time->setDisplayFormat(QStringLiteral("HH:mm"));
    top->addWidget(m_sameTime);
    top->addWidget(m_time);
    top->addStretch(1);
    v->addLayout(top);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(4);
    static const char* names[] = {"Seg", "Ter", "Qua", "Qui", "Sex", "Sáb", "Dom"};
    for (int i = 0; i < 7; ++i) {
        m_days[i] = new QCheckBox(QString::fromUtf8(names[i]));
        m_days[i]->setChecked(i < 6);
        m_dayTimes[i] = new QTimeEdit(QTime(22, 0));
        m_dayTimes[i]->setDisplayFormat(QStringLiteral("HH:mm"));
        grid->addWidget(m_days[i], 0, i, Qt::AlignHCenter);
        grid->addWidget(m_dayTimes[i], 1, i);
        connect(m_days[i], &QCheckBox::toggled, this, [this] { updateMode(); });
    }
    v->addLayout(grid);
    auto* hint = ui::label(QStringLiteral("Desmarque \"mesmo horário\" para escolher um horário por dia (ex.: domingo às 12:00)."),
                           "muted");
    hint->setWordWrap(true);
    v->addWidget(hint);

    connect(m_sameTime, &QCheckBox::toggled, this, [this](bool same) {
        if (!same)
            for (auto* t : m_dayTimes)
                t->setTime(m_time->time());
        updateMode();
    });
    updateMode();
}

void WeekScheduleEditor::updateMode()
{
    const bool same = m_sameTime->isChecked();
    m_time->setEnabled(same);
    for (int i = 0; i < 7; ++i) {
        m_dayTimes[i]->setVisible(!same);
        m_dayTimes[i]->setEnabled(m_days[i]->isChecked());
    }
}

std::vector<ScheduleTrigger> WeekScheduleEditor::triggers() const
{
    std::vector<std::pair<int, int>> dayMinutes;
    for (int i = 0; i < 7; ++i) {
        if (!m_days[i]->isChecked())
            continue;
        const QTime t = m_sameTime->isChecked() ? m_time->time() : m_dayTimes[i]->time();
        dayMinutes.emplace_back(i + 1, t.hour() * 60 + t.minute());
    }
    return triggersFromDayTimes(dayMinutes);
}
