#include "AppContext.h"
#include "Format.h"
#include "Icons.h"
#include "Pages.h"
#include "Theme.h"

#include "config/PlanDescriber.h"
#include "platform/windows/Win32.h"

#include <QGridLayout>
#include <QHBoxLayout>

using namespace ui;

DashboardPage::DashboardPage() : Page(QStringLiteral("Painel"), QStringLiteral("Situação dos backups desta máquina"))
{
    auto* refreshBtn = button(QStringLiteral("Atualizar"), QStringLiteral("refresh"));
    connect(refreshBtn, &QPushButton::clicked, [] { AppContext::instance().reload(); });
    actions()->addWidget(refreshBtn);
}

void DashboardPage::refresh()
{
    clearLayout(body());
    auto& ctx = AppContext::instance();
    const auto& set = ctx.config();
    const auto& t = tk();

    // ---- Faixa de situação ---------------------------------------------------
    const int level = ctx.severityLevel();
    auto* banner = card("banner");
    const QColor bg = level == 2 ? t.errBg : level == 1 ? t.warnBg : t.okBg;
    const QColor fg = level == 2 ? t.errFg : level == 1 ? t.warnFg : t.okFg;
    banner->setStyleSheet(QStringLiteral("QFrame#banner { background:%1; border:none; }").arg(bg.name()));
    auto* bl = new QHBoxLayout(banner);
    bl->setContentsMargins(22, 18, 22, 18);
    bl->setSpacing(16);
    auto* bicon = new QLabel;
    bicon->setPixmap(icons::pixmap(level == 0 ? "shield-check" : "alert", fg, 36));
    bl->addWidget(bicon);
    auto* btext = new QVBoxLayout;
    QString title = level == 0 ? QStringLiteral("Todos os backups em dia")
                               : level == 1 ? QStringLiteral("Há pontos de atenção") : QStringLiteral("Um backup precisa de atenção agora");
    QString detail = QStringLiteral("Nenhum alerta aberto.");
    if (!ctx.alerts().empty())
        detail = fmt::q(ctx.alerts().front().message)
                 + (ctx.alerts().size() > 1 ? QStringLiteral("  (+%1)").arg(ctx.alerts().size() - 1) : QString());
    if (set.jobs.empty())
        detail = QStringLiteral("Nenhuma tarefa configurada ainda. Comece adicionando um disco em Cofres e criando uma tarefa.");
    auto* ht = label(title, "h2");
    ht->setStyleSheet(QStringLiteral("color:%1;").arg(fg.name()));
    auto* dt = label(detail);
    dt->setStyleSheet(QStringLiteral("color:%1;").arg(fg.name()));
    btext->addWidget(ht);
    btext->addWidget(dt);
    bl->addLayout(btext, 1);
    body()->addWidget(banner);

    if (!ctx.stateError().isEmpty())
        body()->addWidget(label(QStringLiteral("Banco de estado indisponível: ") + ctx.stateError(), "muted"));

    // ---- Tarefas ---------------------------------------------------------------
    body()->addWidget(label(QStringLiteral("TAREFAS"), "caption"));
    auto* grid = new QGridLayout;
    grid->setSpacing(18);
    int i = 0;
    for (const auto& job : set.jobs) {
        auto* c = card();
        auto* v = new QVBoxLayout(c);
        v->setContentsMargins(20, 18, 20, 18);
        v->setSpacing(10);

        auto* head = new QHBoxLayout;
        head->setSpacing(12);
        head->addWidget(iconChip(job.type == "database" ? "database" : "folder", 42));
        auto* names = new QVBoxLayout;
        names->setSpacing(0);
        names->addWidget(label(fmt::q(job.name), "h2"));
        QString sub = job.type == "database" ? QStringLiteral("Banco · ") + fmt::q(job.database.engine)
                                             : QStringLiteral("Arquivos · ") + fmt::q(job.files.source);
        if (job.type == "files" && !job.files.moreSources.empty())
            sub += QStringLiteral(" + %1 pasta(s)").arg(job.files.moreSources.size());
        names->addWidget(label(sub, "muted"));
        head->addLayout(names, 1);

        std::optional<synkro::RunRow> last, ok;
        if (auto* st = ctx.state()) {
            last = st->lastRun(synkro::win::fromUtf8(job.id));
            ok = st->lastSuccess(synkro::win::fromUtf8(job.id));
        }
        head->addWidget(last ? pill(fmt::statusText(last->status), fmt::statusTone(last->status))
                             : pill(QStringLiteral("Nunca executada"), Tone::Neutral),
                        0, Qt::AlignTop);
        v->addLayout(head);

        auto* facts = new QGridLayout;
        facts->setHorizontalSpacing(24);
        facts->setVerticalSpacing(4);
        auto fact = [&](int row, int col, const QString& k, const QString& val) {
            auto* box = new QVBoxLayout;
            box->setSpacing(0);
            box->addWidget(label(k, "muted"));
            box->addWidget(label(val, "fact"));
            facts->addLayout(box, row, col);
        };
        fact(0, 0, QStringLiteral("Último sucesso"), ok ? fmt::when(ok->finishedAt) : QStringLiteral("—"));
        fact(0, 1, QStringLiteral("Agendamento"),
             job.schedule.empty() ? QStringLiteral("Sob demanda")
                                  : fmt::q(synkro::config::describeTrigger(job.schedule.front())).remove('.'));
        fact(1, 0, QStringLiteral("Cofres"), QString::number(job.vaults.size()));
        fact(1, 1, QStringLiteral("Situação"), job.enabled ? QStringLiteral("Ativa") : QStringLiteral("Desativada"));
        v->addLayout(facts);
        if (last && !last->error.empty())
            v->addWidget(label(fmt::q(last->error), "muted"));

        auto* run = button(QStringLiteral("Executar agora"), QStringLiteral("play"));
        const QString id = fmt::q(job.id);
        connect(run, &QPushButton::clicked, this, [this, id] { emit runJobRequested(id); });
        auto* row = new QHBoxLayout;
        row->addStretch(1);
        row->addWidget(run);
        v->addLayout(row);

        grid->addWidget(c, i / 2, i % 2);
        ++i;
    }
    if (set.jobs.empty())
        grid->addWidget(label(QStringLiteral("Nenhuma tarefa ainda."), "muted"), 0, 0);
    body()->addLayout(grid);

    // ---- Alertas ----------------------------------------------------------------
    body()->addWidget(label(QStringLiteral("ALERTAS"), "caption"));
    auto* ac = card();
    auto* al = new QVBoxLayout(ac);
    al->setContentsMargins(20, 16, 20, 16);
    al->setSpacing(12);
    if (ctx.alerts().empty())
        al->addWidget(label(QStringLiteral("Nenhum alerta aberto."), "muted"));
    for (const auto& a : ctx.alerts()) {
        auto* row = new QHBoxLayout;
        row->setSpacing(12);
        row->addWidget(badge(a.severity == synkro::AlertSeverity::Critical ? Tone::Error : Tone::Warn), 0, Qt::AlignTop);
        auto* txt = new QVBoxLayout;
        txt->setSpacing(0);
        txt->addWidget(label(fmt::q(a.message)));
        txt->addWidget(label(QStringLiteral("Desde %1").arg(fmt::when(a.openedAt)), "muted"));
        row->addLayout(txt, 1);
        al->addLayout(row);
    }
    body()->addWidget(ac);
}
