#include "AppContext.h"
#include "Format.h"
#include "Pages.h"
#include "Theme.h"

#include "database/DatabaseRestore.h"
#include "database/DumpStore.h"
#include "engine/Restore.h"
#include "engine/RunSummary.h"
#include "engine/VaultOps.h"
#include "platform/windows/Win32.h"
#include "vault/JobLayout.h"
#include "vault/VaultLocator.h"

#include <QFileDialog>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QMessageBox>
#include <QRegularExpression>
#include <QtConcurrent>

using namespace ui;

namespace {

// Nome amigável da tarefa a partir do job.yml gravado no cofre.
QString jobName(const std::wstring& vaultLocation, const std::wstring& jobId)
{
    auto data = synkro::win::readWholeFile(synkro::vault::JobLayout(vaultLocation, jobId).jobConfigFile(), 1u << 20);
    if (data) {
        const QRegularExpression re(QStringLiteral("^name:\\s*\"?([^\"\\n]+)\"?\\s*$"), QRegularExpression::MultilineOption);
        const auto m = re.match(QString::fromStdString(data.value()));
        if (m.hasMatch())
            return m.captured(1).trimmed();
    }
    return fmt::q(jobId);
}

} // namespace

RestorePage::RestorePage()
    : Page(QStringLiteral("Restaurar"),
           QStringLiteral("Recupera arquivos ou bancos de qualquer cofre conectado — inclusive de outro computador"))
{
    auto* reload = button(QStringLiteral("Procurar cofres"), QStringLiteral("refresh"));
    connect(reload, &QPushButton::clicked, this, [this] { refresh(); });
    actions()->addWidget(reload);

    auto* pick = card();
    auto* g = new QGridLayout(pick);
    g->setContentsMargins(22, 20, 22, 20);
    g->setHorizontalSpacing(16);
    g->setVerticalSpacing(10);
    g->addWidget(label(QStringLiteral("Cofre"), "fact"), 0, 0);
    m_vault = new QComboBox;
    g->addWidget(m_vault, 0, 1);
    g->addWidget(label(QStringLiteral("Tarefa"), "fact"), 1, 0);
    m_job = new QComboBox;
    g->addWidget(m_job, 1, 1);
    g->setColumnStretch(1, 1);
    body()->addWidget(pick);

    connect(m_vault, &QComboBox::currentIndexChanged, this, [this] { loadJobs(); });
    connect(m_job, &QComboBox::currentIndexChanged, this, [this] { loadPoints(); });

    body()->addWidget(label(QStringLiteral("PONTOS DE RESTAURAÇÃO"), "caption"));
    m_points = new QTableWidget;
    m_points->verticalHeader()->hide();
    m_points->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_points->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_points->setSelectionMode(QAbstractItemView::SingleSelection);
    m_points->setAlternatingRowColors(true);
    m_points->setShowGrid(false);
    m_points->setMinimumHeight(260);
    body()->addWidget(m_points);

    auto* target = card();
    auto* tv = new QVBoxLayout(target);
    tv->setContentsMargins(22, 20, 22, 20);
    tv->setSpacing(10);
    tv->addWidget(label(QStringLiteral("Voltar para este ponto"), "h2"));
    m_hint = label(QString(), "muted");
    tv->addWidget(m_hint);

    m_filesTarget = new QWidget;
    auto* fg = new QGridLayout(m_filesTarget);
    fg->setContentsMargins(0, 0, 0, 0);
    fg->addWidget(label(QStringLiteral("Pasta de destino (nova ou vazia)")), 0, 0);
    m_targetFolder = new QLineEdit;
    fg->addWidget(m_targetFolder, 0, 1);
    auto* browse = button(QStringLiteral("Escolher…"), QStringLiteral("folder"));
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, QStringLiteral("Pasta de destino"));
        if (!d.isEmpty())
            m_targetFolder->setText(QDir::toNativeSeparators(d));
    });
    fg->addWidget(browse, 0, 2);
    fg->addWidget(label(QStringLiteral("Somente esta pasta/arquivo (opcional)")), 1, 0);
    m_pathFilter = new QLineEdit;
    m_pathFilter->setPlaceholderText(QStringLiteral("ex.: NFe/2026"));
    fg->addWidget(m_pathFilter, 1, 1, 1, 2);
    fg->setColumnStretch(1, 1);
    tv->addWidget(m_filesTarget);

    m_dbTarget = new QWidget;
    auto* dg = new QGridLayout(m_dbTarget);
    dg->setContentsMargins(0, 0, 0, 0);
    dg->addWidget(label(QStringLiteral("Banco NOVO (PostgreSQL/MySQL) ou arquivo novo (SQLite/personalizado)")), 0, 0);
    m_targetDb = new QLineEdit;
    m_targetDb->setPlaceholderText(QStringLiteral("ex.: pdv_vendas_restaurado  ou  D:\\Restaurado\\caixa.db"));
    dg->addWidget(m_targetDb, 0, 1);
    dg->setColumnStretch(1, 1);
    tv->addWidget(m_dbTarget);

    auto* go = button(QStringLiteral("Restaurar"), QStringLiteral("restore"), true);
    connect(go, &QPushButton::clicked, this, [this] { restore(); });
    tv->addWidget(go, 0, Qt::AlignLeft);
    body()->addWidget(target);
}

void RestorePage::refresh()
{
    const QString previous = m_vault->currentData().toString();
    m_vault->blockSignals(true);
    m_vault->clear();
    for (const auto& v : synkro::vault::discoverVaults())
        m_vault->addItem(v.identity.label + QStringLiteral("  —  ") + fmt::q(v.location), fmt::q(v.location));
    // Cofres-pasta declarados na configuração (não aparecem na varredura de volumes).
    for (const auto& obs : AppContext::instance().vaults()) {
        if (obs.connected && m_vault->findData(fmt::q(obs.location)) < 0) {
            const auto* vc = AppContext::instance().config().global.findVault(obs.id);
            m_vault->addItem((vc ? fmt::q(vc->label) : fmt::q(obs.id)) + QStringLiteral("  —  ") + fmt::q(obs.location),
                             fmt::q(obs.location));
        }
    }
    const int idx = m_vault->findData(previous);
    m_vault->setCurrentIndex(idx >= 0 ? idx : 0);
    m_vault->blockSignals(false);
    if (m_vault->count() == 0)
        m_hint->setText(QStringLiteral("Nenhum cofre conectado. Conecte um disco de backup e clique em Procurar cofres."));
    loadJobs();
}

void RestorePage::loadJobs()
{
    m_job->blockSignals(true);
    m_job->clear();
    const std::wstring location = m_vault->currentData().toString().toStdWString();
    if (!location.empty()) {
        const std::wstring jobsDir = synkro::win::joinPath(synkro::vault::vaultRoot(location), L"jobs");
        for (const auto& id : synkro::ops::listSubdirectories(jobsDir))
            m_job->addItem(jobName(location, id) + QStringLiteral("  (") + fmt::q(id) + QStringLiteral(")"), fmt::q(id));
    }
    m_job->blockSignals(false);
    loadPoints();
}

void RestorePage::loadPoints()
{
    m_points->clear();
    m_points->setRowCount(0);
    const std::wstring location = m_vault->currentData().toString().toStdWString();
    const std::wstring jobId = m_job->currentData().toString().toStdWString();
    if (location.empty() || jobId.empty()) {
        m_filesTarget->hide();
        m_dbTarget->hide();
        return;
    }
    const synkro::vault::JobLayout layout(location, jobId);
    const auto dumps = synkro::db::listDumps(layout);
    m_isDatabase = !dumps.empty();
    m_filesTarget->setVisible(!m_isDatabase);
    m_dbTarget->setVisible(m_isDatabase);

    if (m_isDatabase) {
        m_points->setColumnCount(5);
        m_points->setHorizontalHeaderLabels({QStringLiteral("Banco"), QStringLiteral("Data"), QStringLiteral("Tamanho"),
                                             QStringLiteral("Tabelas"), QStringLiteral("Situação")});
        m_points->setRowCount(static_cast<int>(dumps.size()));
        int r = 0;
        for (const auto& d : dumps) {
            auto* first = new QTableWidgetItem(fmt::q(d.database));
            first->setData(Qt::UserRole, fmt::q(d.runId));
            first->setData(Qt::UserRole + 1, fmt::q(d.database));
            first->setData(Qt::UserRole + 2, fmt::q(d.engine));
            m_points->setItem(r, 0, first);
            m_points->setItem(r, 1, new QTableWidgetItem(fmt::when(d.startedAt)));
            m_points->setItem(r, 2, new QTableWidgetItem(fmt::bytes(d.size)));
            m_points->setItem(r, 3, new QTableWidgetItem(d.tablesKnown ? QString::number(d.tables.size()) : QStringLiteral("—")));
            m_points->setItem(r, 4, new QTableWidgetItem(d.validationResult == "ok" ? (d.suspicious ? QStringLiteral("Válido · suspeito")
                                                                                                     : QStringLiteral("Válido"))
                                                                                   : QStringLiteral("Inválido")));
            ++r;
        }
        m_hint->setText(QStringLiteral("O banco é sempre restaurado com um nome NOVO; o banco em uso nunca é substituído. "
                                       "O SHA-256 do dump é conferido antes."));
    } else {
        m_points->setColumnCount(4);
        m_points->setHorizontalHeaderLabels(
            {QStringLiteral("Execução"), QStringLiteral("Data"), QStringLiteral("Mudanças"), QStringLiteral("Situação")});
        auto runs = synkro::listRuns(layout);
        std::reverse(runs.begin(), runs.end());
        m_points->setRowCount(static_cast<int>(runs.size()));
        int r = 0;
        for (const auto& id : runs) {
            auto* first = new QTableWidgetItem(fmt::q(id));
            first->setData(Qt::UserRole, fmt::q(id));
            m_points->setItem(r, 0, first);
            if (auto s = synkro::RunSummary::load(layout.summaryFile(id))) {
                m_points->setItem(r, 1, new QTableWidgetItem(fmt::when(s.value().finishedAt)));
                m_points->setItem(r, 2, new QTableWidgetItem(QStringLiteral("+%1  ~%2  −%3")
                                                                 .arg(s.value().added).arg(s.value().modified).arg(s.value().deleted)));
                m_points->setItem(r, 3, new QTableWidgetItem(fmt::statusText(s.value().status.toStdString())));
            }
            ++r;
        }
        m_hint->setText(QStringLiteral("Os arquivos voltam para uma pasta NOVA (ou vazia) exatamente como estavam na execução "
                                       "escolhida; cada arquivo é conferido por SHA-256. Nada do que está em uso é substituído."));
    }
    m_points->resizeColumnsToContents();
    m_points->horizontalHeader()->setStretchLastSection(true);
    if (m_points->rowCount() > 0)
        m_points->selectRow(0);
}

void RestorePage::restore()
{
    auto* item = m_points->currentRow() >= 0 ? m_points->item(m_points->currentRow(), 0) : nullptr;
    if (!item) {
        QMessageBox::information(this, QStringLiteral("Restaurar"), QStringLiteral("Escolha um ponto de restauração."));
        return;
    }
    const std::wstring location = m_vault->currentData().toString().toStdWString();
    const std::wstring jobId = m_job->currentData().toString().toStdWString();
    const std::wstring runId = item->data(Qt::UserRole).toString().toStdWString();

    auto* watcher = new QFutureWatcher<QString>(this);
    if (!m_isDatabase) {
        if (m_targetFolder->text().trimmed().isEmpty()) {
            QMessageBox::information(this, QStringLiteral("Restaurar"), QStringLiteral("Escolha a pasta de destino."));
            return;
        }
        synkro::RestoreRequest req;
        req.vaultLocation = location;
        req.jobId = jobId;
        req.runId = runId;
        req.targetDirectory = QDir::toNativeSeparators(m_targetFolder->text().trimmed()).toStdWString();
        req.pathPrefix = m_pathFilter->text().trimmed().toStdWString();
        watcher->setFuture(QtConcurrent::run([req]() -> QString {
            auto r = synkro::restore(req);
            if (!r)
                return QStringLiteral("ERRO: ") + fmt::q(r.error().message);
            QString msg = QStringLiteral("%1 arquivo(s) restaurado(s) (%2), todos conferidos por SHA-256.")
                              .arg(r.value().restored).arg(fmt::bytes(r.value().bytes));
            for (const auto& [p, e] : r.value().failures)
                msg += QStringLiteral("\n• %1: %2").arg(fmt::q(p), fmt::q(e));
            return msg;
        }));
    } else {
        const std::string engine = item->data(Qt::UserRole + 2).toString().toStdString();
        const QString target = m_targetDb->text().trimmed();
        if (target.isEmpty()) {
            QMessageBox::information(this, QStringLiteral("Restaurar"), QStringLiteral("Informe o destino."));
            return;
        }
        synkro::db::DatabaseRestoreRequest req;
        req.vaultLocation = location;
        req.jobId = jobId;
        req.database = item->data(Qt::UserRole + 1).toString().toStdString();
        req.runId = runId;
        if (engine == "sqlite" || engine == "custom" || engine == "firebird")
            req.targetPath = QDir::toNativeSeparators(target).toStdWString();
        else
            req.targetDatabase = target.toStdString();
        if (const auto* job = AppContext::instance().config().findJob(synkro::win::toUtf8(jobId)); job && job->type == "database")
            req.job = job->database;
        else if (engine != "sqlite")
            req.job.engine = engine; // sem conexão configurada: a restauração explicará o que falta
        watcher->setFuture(QtConcurrent::run([req]() -> QString {
            auto r = synkro::db::restoreDatabase(req);
            if (!r)
                return QStringLiteral("ERRO: ") + fmt::q(r.error().message);
            return QStringLiteral("Dump de %1 restaurado em %2 (SHA-256 conferido).").arg(fmt::q(r.value().dump.database), fmt::q(r.value().target));
        }));
    }
    setEnabled(false);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        setEnabled(true);
        const QString msg = watcher->result();
        if (msg.startsWith(QStringLiteral("ERRO: ")))
            QMessageBox::warning(this, QStringLiteral("Restauração"), msg.mid(6));
        else
            QMessageBox::information(this, QStringLiteral("Restauração concluída"), msg);
        watcher->deleteLater();
    });
}
