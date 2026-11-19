#include "AppContext.h"
#include "Dialogs.h"
#include "Format.h"
#include "Icons.h"
#include "Pages.h"
#include "Theme.h"

#include "config/Values.h"
#include "engine/Verify.h"
#include "platform/windows/Volume.h"
#include "platform/windows/Win32.h"
#include "vault/JobLayout.h"
#include "vault/Vault.h"

#include <QDesktopServices>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QUrl>

using namespace ui;

VaultsPage::VaultsPage()
    : Page(QStringLiteral("Cofres"), QStringLiteral("Para onde vão as cópias. Cada cofre fica num disco físico diferente."))
{
    auto* add = button(QStringLiteral("Adicionar disco"), QStringLiteral("plus"), true);
    connect(add, &QPushButton::clicked, this, [this] {
        AddVaultDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted)
            AppContext::instance().reload();
    });
    actions()->addWidget(add);
}

void VaultsPage::refresh()
{
    clearLayout(body());
    auto& ctx = AppContext::instance();
    const auto& set = ctx.config();

    // ---- Mapa de redundância --------------------------------------------------
    auto* map = card();
    auto* ml = new QHBoxLayout(map);
    ml->setContentsMargins(26, 24, 26, 24);
    ml->setSpacing(0);

    auto* sources = new QVBoxLayout;
    sources->setSpacing(10);
    sources->addWidget(label(QStringLiteral("ORIGENS"), "caption"));
    for (const auto& j : set.jobs) {
        auto* box = card("insetBox");
        auto* bl = new QVBoxLayout(box);
        bl->setContentsMargins(14, 10, 14, 10);
        bl->setSpacing(2);
        QString title, sub;
        if (j.type == "files") {
            title = fmt::q(j.files.source);
            sub = fmt::q(j.name);
        } else {
            title = fmt::q(j.database.engine) + (j.database.host.empty() ? QString() : QStringLiteral(" · ") + fmt::q(j.database.host));
            sub = fmt::q(j.name);
        }
        bl->addWidget(label(title, "fact"));
        bl->addWidget(label(sub, "muted"));
        sources->addWidget(box);
    }
    if (set.jobs.empty())
        sources->addWidget(label(QStringLiteral("Nenhuma tarefa configurada."), "muted"));
    sources->addStretch(1);
    ml->addLayout(sources, 3);

    auto* arrows = new QVBoxLayout;
    arrows->setContentsMargins(14, 30, 14, 0);
    arrows->setSpacing(52);
    auto* targets = new QVBoxLayout;
    targets->setSpacing(14);
    targets->addWidget(label(QStringLiteral("COFRES"), "caption"));
    for (const auto& obs : ctx.vaults()) {
        const auto* vc = set.global.findVault(obs.id);
        if (!vc)
            continue;
        arrows->addWidget(new Arrow(!obs.connected));
        auto* box = card(obs.connected ? "vaultOn" : "vaultOff");
        auto* bl = new QVBoxLayout(box);
        bl->setContentsMargins(16, 12, 16, 12);
        bl->setSpacing(2);
        auto* top = new QHBoxLayout;
        top->addWidget(label(fmt::q(vc->label), "fact"), 1);
        auto* state = label(obs.connected ? QStringLiteral("● conectado") : (vc->rotation ? QStringLiteral("○ guardado fora")
                                                                                          : QStringLiteral("○ desconectado")));
        state->setStyleSheet(QStringLiteral("color:%1; font-weight:600; font-size:8.5pt;")
                                 .arg((obs.connected ? tk().okFg : tk().muted).name()));
        top->addWidget(state);
        bl->addLayout(top);
        QString sub = vc->rotation ? QStringLiteral("rodízio · limite de ausência ") : QStringLiteral("fixo · limite de ausência ");
        sub += fmt::q(synkro::config::describeDuration(vc->maxAbsenceSeconds));
        bl->addWidget(label(sub, "muted"));
        targets->addWidget(box);
    }
    if (ctx.vaults().empty())
        targets->addWidget(label(QStringLiteral("Nenhum cofre cadastrado."), "muted"));
    arrows->addStretch(1);
    targets->addStretch(1);
    ml->addLayout(arrows, 2);
    ml->addLayout(targets, 3);
    body()->addWidget(map);

    // ---- Cartões dos discos -----------------------------------------------------
    auto* grid = new QGridLayout;
    grid->setSpacing(20);
    int i = 0;
    for (const auto& obs : ctx.vaults()) {
        const auto* vc = set.global.findVault(obs.id);
        if (!vc)
            continue;
        auto* c = card();
        auto* v = new QVBoxLayout(c);
        v->setContentsMargins(22, 20, 22, 20);
        v->setSpacing(14);

        auto* head = new QHBoxLayout;
        head->setSpacing(14);
        head->addWidget(iconChip(QStringLiteral("disk")));
        auto* names = new QVBoxLayout;
        names->setSpacing(0);
        names->addWidget(label(fmt::q(vc->label), "h2"));
        QString model = QStringLiteral("desconectado");
        synkro::win::VolumeInfo vol;
        if (obs.connected) {
            if (auto info = synkro::win::queryVolume(obs.location)) {
                vol = info.value();
                QStringList disks;
                for (auto d : vol.physicalDisks)
                    disks << QString::number(d);
                model = fmt::q(vol.fileSystem) + QStringLiteral(" · Disco ") + disks.join(',') + QStringLiteral(" · ")
                        + (vol.kind == synkro::win::DriveKind::Removable ? QStringLiteral("removível") : QStringLiteral("fixo"));
            }
        }
        names->addWidget(label(model, "muted"));
        head->addLayout(names, 1);
        v->addLayout(head);

        if (obs.connected && obs.totalBytes > 0) {
            const double used = 1.0 - static_cast<double>(obs.freeBytes) / static_cast<double>(obs.totalBytes);
            auto* row = new QHBoxLayout;
            row->addWidget(label(QStringLiteral("Espaço usado"), "muted"), 1);
            row->addWidget(label(fmt::bytes(obs.totalBytes - obs.freeBytes) + QStringLiteral(" de ") + fmt::bytes(obs.totalBytes), "fact"));
            v->addLayout(row);
            v->addWidget(new UsageBar(used));
        }

        auto* facts = new QGridLayout;
        facts->setHorizontalSpacing(24);
        facts->setVerticalSpacing(10);
        auto fact = [&](int idx, const QString& k, const QString& val) {
            auto* box = new QVBoxLayout;
            box->setSpacing(0);
            box->addWidget(label(k, "muted"));
            box->addWidget(label(val, "fact"));
            facts->addLayout(box, idx / 2, idx % 2);
        };
        std::optional<synkro::VaultSeenRow> seen;
        if (auto* st = ctx.state())
            seen = st->vaultSeen(obs.vaultId);
        fact(0, QStringLiteral("Último backup"), seen ? fmt::when(seen->lastSuccessAt) : QStringLiteral("—"));
        fact(1, QStringLiteral("Visto pela última vez"), obs.connected ? QStringLiteral("Agora") : seen ? fmt::when(seen->lastSeenAt) : QStringLiteral("Nunca"));
        fact(2, QStringLiteral("Local"), obs.connected ? fmt::q(obs.location) : QStringLiteral("—"));
        fact(3, QStringLiteral("Identidade"), obs.vaultId.left(4) + QStringLiteral("…") + obs.vaultId.right(4));
        v->addLayout(facts);

        auto* buttons = new QHBoxLayout;
        auto* verify = button(QStringLiteral("Verificar agora"), QStringLiteral("check"));
        verify->setEnabled(obs.connected);
        const std::wstring location = obs.location;
        const std::string vaultCfgId = obs.id;
        connect(verify, &QPushButton::clicked, this, [this, location, vaultCfgId] {
            QString report;
            bool healthy = true;
            for (const auto& j : AppContext::instance().config().jobs) {
                if (j.type != "files" || std::find(j.vaults.begin(), j.vaults.end(), vaultCfgId) == j.vaults.end())
                    continue;
                auto r = synkro::verifyVault(synkro::vault::JobLayout(location, synkro::win::fromUtf8(j.id)), 100.0);
                if (!r) {
                    report += fmt::q(j.name) + QStringLiteral(": ") + fmt::q(r.error().message) + "\n";
                    continue;
                }
                healthy = healthy && r.value().healthy();
                report += QStringLiteral("%1: %2 de %3 arquivos íntegros\n")
                              .arg(fmt::q(j.name)).arg(r.value().ok).arg(r.value().checked);
            }
            if (report.isEmpty())
                report = QStringLiteral("Nenhuma tarefa de arquivos usa este cofre.");
            if (healthy)
                QMessageBox::information(this, QStringLiteral("Verificação do cofre"), report);
            else
                QMessageBox::warning(this, QStringLiteral("Verificação do cofre"), report);
        });
        auto* open = button(QStringLiteral("Abrir no Explorer"), QStringLiteral("external"));
        open->setEnabled(obs.connected);
        connect(open, &QPushButton::clicked, [location] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(fmt::q(synkro::vault::vaultRoot(location))));
        });
        buttons->addWidget(verify);
        buttons->addWidget(open);
        buttons->addStretch(1);
        v->addLayout(buttons);
        v->addStretch(1);

        grid->addWidget(c, i / 2, i % 2);
        ++i;
    }

    // Cartão para adicionar um disco.
    auto* add = card("addCard");
    auto* av = new QVBoxLayout(add);
    av->setContentsMargins(22, 20, 22, 20);
    av->setSpacing(10);
    av->addWidget(label(QStringLiteral("Adicionar um disco"), "h2"));
    av->addWidget(label(QStringLiteral("Antes de virar cofre, o disco passa por uma checagem: precisa ser um disco físico "
                                       "diferente da origem e dos outros cofres, ter sistema de arquivos adequado, espaço "
                                       "suficiente e passar num teste de gravação e releitura."),
                        "muted"));
    av->addStretch(1);
    auto* addBtn = button(QStringLiteral("Escolher disco"), QStringLiteral("plus"), true);
    connect(addBtn, &QPushButton::clicked, this, [this] {
        AddVaultDialog dlg(this);
        if (dlg.exec() == QDialog::Accepted)
            AppContext::instance().reload();
    });
    av->addWidget(addBtn, 0, Qt::AlignLeft);
    grid->addWidget(add, i / 2, i % 2);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    body()->addLayout(grid);
}
