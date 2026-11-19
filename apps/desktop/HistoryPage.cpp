#include "AppContext.h"
#include "Format.h"
#include "Pages.h"
#include "Theme.h"

#include "platform/windows/Win32.h"

#include <QHeaderView>

using namespace ui;

HistoryPage::HistoryPage() : Page(QStringLiteral("Histórico"), QStringLiteral("Execuções registradas nesta máquina"))
{
    m_table = new QTableWidget;
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Início"), QStringLiteral("Tarefa"), QStringLiteral("Tipo"),
                                        QStringLiteral("Disparo"), QStringLiteral("Situação"), QStringLiteral("Detalhe")});
    m_table->verticalHeader()->hide();
    m_table->setAlternatingRowColors(true);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_table->setMinimumHeight(520);
    m_table->setShowGrid(false);
    body()->addWidget(m_table);
}

void HistoryPage::refresh()
{
    m_table->setRowCount(0);
    auto* st = AppContext::instance().state();
    if (!st)
        return;
    const auto runs = st->recentRuns(300);
    m_table->setRowCount(static_cast<int>(runs.size()));
    int r = 0;
    for (const auto& run : runs) {
        auto set = [&](int col, const QString& text) {
            auto* it = new QTableWidgetItem(text);
            m_table->setItem(r, col, it);
            return it;
        };
        set(0, fmt::when(run.startedAt));
        const auto* job = AppContext::instance().config().findJob(synkro::win::toUtf8(run.jobId));
        set(1, job ? fmt::q(job->name) : fmt::q(run.jobId));
        set(2, run.type == "database" ? QStringLiteral("Banco") : QStringLiteral("Arquivos"));
        set(3, run.trigger == "schedule" ? QStringLiteral("Agendado") : QStringLiteral("Manual"));
        auto* status = set(4, fmt::statusText(run.status));
        const auto tone = fmt::statusTone(run.status);
        status->setForeground(tone == Tone::Ok ? tk().okFg : tone == Tone::Warn ? tk().warnFg : tone == Tone::Error ? tk().errFg : tk().muted);
        const qint64 secs = run.startedAt.secsTo(run.finishedAt);
        set(5, run.error.empty() ? QStringLiteral("duração %1 s").arg(secs) : fmt::q(run.error));
        ++r;
    }
    m_table->resizeColumnsToContents();
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
}
