#include "AppContext.h"
#include "Dialogs.h"
#include "FlowEditor.h"
#include "Format.h"
#include "Pages.h"
#include "Theme.h"

#include "config/ConfigWriter.h"
#include "config/PlanDescriber.h"
#include "platform/windows/Win32.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QMessageBox>
#include <QTextBrowser>

using namespace ui;

JobsPage::JobsPage() : Page(QStringLiteral("Tarefas"), QStringLiteral("O que é copiado, quando e para quais cofres"))
{
    auto* add = button(QStringLiteral("Nova tarefa"), QStringLiteral("plus"), true);
    connect(add, &QPushButton::clicked, this, [this] {
        JobWizard wizard(this);
        if (wizard.exec() == QDialog::Accepted)
            AppContext::instance().reload();
    });
    actions()->addWidget(add);

    m_tree = new QTreeWidget;
    m_tree->setColumnCount(6);
    m_tree->setHeaderLabels({QStringLiteral("Tarefa"), QStringLiteral("Tipo"), QStringLiteral("Agendamento"),
                             QStringLiteral("Cofres"), QStringLiteral("Última execução"), QStringLiteral("Situação")});
    m_tree->setRootIsDecorated(false);
    m_tree->setAlternatingRowColors(true);
    m_tree->setMinimumHeight(320);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this] { editSelected(); });
    body()->addWidget(m_tree);

    auto* row = new QHBoxLayout;
    auto* run = button(QStringLiteral("Executar agora"), QStringLiteral("play"));
    auto* plan = button(QStringLiteral("O que esta tarefa vai fazer"), QStringLiteral("jobs"));
    auto* edit = button(QStringLiteral("Editar"), QStringLiteral("edit"));
    connect(run, &QPushButton::clicked, this, [this] {
        if (!selectedJob().isEmpty())
            emit runJobRequested(selectedJob());
    });
    connect(plan, &QPushButton::clicked, this, [this] { showPlan(); });
    connect(edit, &QPushButton::clicked, this, [this] { editSelected(); });
    row->addWidget(run);
    row->addWidget(plan);
    row->addWidget(edit);
    row->addStretch(1);
    body()->addLayout(row);

    body()->addWidget(label(QStringLiteral("Editar abre o modo fácil (blocos ligados, como num fluxo) ou o editor YAML, à "
                                           "sua escolha. Os dois validam antes de gravar."),
                            "muted"));
}

QString JobsPage::selectedJob() const
{
    auto* item = m_tree->currentItem();
    return item ? item->data(0, Qt::UserRole).toString() : QString();
}

void JobsPage::refresh()
{
    m_tree->clear();
    auto& ctx = AppContext::instance();
    for (const auto& job : ctx.config().jobs) {
        auto* item = new QTreeWidgetItem(m_tree);
        item->setData(0, Qt::UserRole, fmt::q(job.id));
        item->setText(0, fmt::q(job.name) + (job.enabled ? QString() : QStringLiteral(" (desativada)")));
        item->setText(1, job.type == "database" ? QStringLiteral("Banco · ") + fmt::q(job.database.engine) : QStringLiteral("Arquivos"));
        QStringList sched;
        for (const auto& t : job.schedule)
            sched << fmt::q(synkro::config::describeTrigger(t)).remove('.');
        item->setText(2, sched.isEmpty() ? QStringLiteral("Sob demanda") : sched.join(QStringLiteral("; ")));
        item->setText(3, QString::number(job.vaults.size()));
        if (auto* st = ctx.state()) {
            if (auto last = st->lastRun(synkro::win::fromUtf8(job.id))) {
                item->setText(4, fmt::when(last->finishedAt));
                item->setText(5, fmt::statusText(last->status));
                const auto tone = fmt::statusTone(last->status);
                const QColor color = tone == Tone::Ok ? tk().okFg : tone == Tone::Warn ? tk().warnFg : tone == Tone::Error ? tk().errFg : tk().muted;
                item->setForeground(5, color);
            } else {
                item->setText(4, QStringLiteral("—"));
                item->setText(5, QStringLiteral("Nunca executada"));
            }
        }
    }
    for (int c = 1; c < 6; ++c)
        if (c != 2)
            m_tree->resizeColumnToContents(c);
}

void JobsPage::editSelected()
{
    const QString id = selectedJob();
    if (id.isEmpty())
        return;
    const auto* job = AppContext::instance().config().findJob(id.toStdString());
    if (!job)
        return;
    const bool handEdited = !synkro::config::isPristineGenerated(AppContext::instance().readConfigFile(job->file));
    QMessageBox ask(this);
    ask.setWindowTitle(QStringLiteral("Editar tarefa"));
    ask.setIcon(QMessageBox::Question);
    ask.setText(QStringLiteral("Como você quer editar \"%1\"?").arg(fmt::q(job->name)));
    QString info = QStringLiteral("Modo fácil: blocos ligados por linhas (quando rodar, comandos antes/depois e para quais "
                                  "cofres vai a cópia).\nEditor YAML: o arquivo completo, com todas as opções.");
    if (handEdited)
        info += QStringLiteral("\n\nEste arquivo foi editado à mão: no modo fácil, o resultado passa pelo editor YAML para "
                               "você conferir antes de gravar (comentários não são mantidos).");
    ask.setInformativeText(info);
    auto* easy = ask.addButton(QStringLiteral("Modo fácil (blocos)"), QMessageBox::AcceptRole);
    auto* yaml = ask.addButton(QStringLiteral("Editor YAML"), QMessageBox::AcceptRole);
    ask.addButton(QStringLiteral("Cancelar"), QMessageBox::RejectRole);
    ask.setDefaultButton(easy);
    ask.exec();
    int result = QDialog::Rejected;
    if (ask.clickedButton() == easy) {
        FlowEditorDialog dlg(*job, this);
        result = dlg.exec();
    } else if (ask.clickedButton() == yaml) {
        YamlEditorDialog dlg(job->file, this);
        result = dlg.exec();
    }
    if (result == QDialog::Accepted)
        AppContext::instance().reload();
}

void JobsPage::showPlan()
{
    const QString id = selectedJob();
    const auto& set = AppContext::instance().config();
    const auto* job = set.findJob(id.toStdString());
    if (!job)
        return;
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("O que esta tarefa vai fazer"));
    dlg.resize(760, 640);
    auto* v = new QVBoxLayout(&dlg);
    auto* text = new QTextBrowser;
    text->setPlainText(fmt::q(synkro::config::describeJob(set, *job)));
    text->setStyleSheet(QStringLiteral("font-family: 'Cascadia Mono', Consolas; font-size: 10pt;"));
    v->addWidget(text);
    auto* close = button(QStringLiteral("Fechar"));
    connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);
    v->addWidget(close, 0, Qt::AlignRight);
    dlg.exec();
}
