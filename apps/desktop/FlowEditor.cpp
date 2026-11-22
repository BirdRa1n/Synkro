#include "FlowEditor.h"

#include "AppContext.h"
#include "Dialogs.h"
#include "Format.h"
#include "Icons.h"
#include "JobWidgets.h"
#include "Theme.h"
#include "Ui.h"

#include "config/ConfigLoader.h"
#include "config/ConfigWriter.h"
#include "config/PlanDescriber.h"
#include "config/Values.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsSceneMouseEvent>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QProcess>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QTimeEdit>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <functional>

using namespace synkro::config;
using flow::NodeKind;
using flow::Port;

namespace {

constexpr double kNodeW = 214;
constexpr double kNodeH = 66;
constexpr double kSourceH = 104;
constexpr double kPortR = 6;
const char* kMime = "application/x-synkro-block";

QString joinCommand(const std::vector<std::string>& run)
{
    QStringList parts;
    for (const auto& a : run) {
        QString s = QString::fromStdString(a);
        if (s.isEmpty() || s.contains(' ') || s.contains('"'))
            s = '"' + QString(s).replace('"', QStringLiteral("\\\"")) + '"';
        parts << s;
    }
    return parts.join(' ');
}

QString weekdayShort(int d)
{
    static const char* names[] = {"seg", "ter", "qua", "qui", "sex", "sáb", "dom"};
    return d >= 1 && d <= 7 ? QString::fromUtf8(names[d - 1]) : QString();
}

QString hhmm(int minutes)
{
    return QStringLiteral("%1:%2").arg(minutes / 60, 2, 10, QChar('0')).arg(minutes % 60, 2, 10, QChar('0'));
}

QString triggerTitle(const ScheduleTrigger& t)
{
    switch (t.kind) {
    case TriggerKind::Daily: return QStringLiteral("Todo dia · ") + hhmm(t.atMinutes);
    case TriggerKind::Weekly: return QStringLiteral("Semanal · ") + hhmm(t.atMinutes);
    case TriggerKind::Monthly: return QStringLiteral("Mensal · ") + hhmm(t.atMinutes);
    case TriggerKind::Every: return QStringLiteral("A cada ") + fmt::q(describeDuration(t.intervalSeconds));
    case TriggerKind::OnStartup: return QStringLiteral("Ao ligar o PC");
    case TriggerKind::OnLogon: return QStringLiteral("Ao entrar no Windows");
    case TriggerKind::Manual: return QStringLiteral("Manual");
    }
    return {};
}

QString triggerSubtitle(const ScheduleTrigger& t)
{
    auto days = [&] {
        QStringList l;
        for (int d : t.days)
            l << weekdayShort(d);
        return l.join(QStringLiteral(", "));
    };
    switch (t.kind) {
    case TriggerKind::Weekly: return days();
    case TriggerKind::Monthly: return t.dayOfMonth < 0 ? QStringLiteral("último dia do mês") : QStringLiteral("dia %1").arg(t.dayOfMonth);
    case TriggerKind::Every: return t.days.empty() ? QStringLiteral("o dia todo") : days();
    case TriggerKind::OnStartup:
    case TriggerKind::OnLogon:
        return t.delaySeconds > 0 ? fmt::q(describeDuration(t.delaySeconds)) + QStringLiteral(" depois") : QStringLiteral("na hora");
    default: return QStringLiteral("gatilho");
    }
}

} // namespace

// ============================================================================
// Cena: blocos, ligações animadas e o arrasto de novas ligações
// ============================================================================

class FlowScene;

class NodeItem : public QGraphicsItem {
public:
    NodeItem(FlowScene* scene, int id);
    int id() const { return m_id; }
    double height() const;
    QRectF boundingRect() const override { return QRectF(-kPortR - 2, -2, kNodeW + 2 * kPortR + 4, height() + 4); }
    void paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*) override;

    bool hasInput() const;
    QPointF inputPoint() const { return QPointF(0, height() / 2); }
    std::vector<Port> outputs() const;
    QPointF outputPoint(Port port) const;
    std::optional<Port> outputAt(QPointF local) const;

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant& value) override;

private:
    FlowScene* m_flow;
    int m_id;
};

class FlowScene : public QGraphicsScene {
public:
    FlowScene(flow::Graph& graph, const JobConfig& job, QObject* parent) : QGraphicsScene(parent), m_graph(graph), m_job(job)
    {
        m_timer.setInterval(33);
        QObject::connect(&m_timer, &QTimer::timeout, this, [this] {
            m_phase = std::fmod(m_phase + 0.01, 1.0);
            update();
        });
        m_timer.start();
    }

    flow::Graph& graph() { return m_graph; }
    const JobConfig& job() const { return m_job; }
    std::function<void()> onChanged;
    std::function<void(int)> onSelected;
    std::function<void(const QString&)> onMessage;

    NodeItem* item(int id) const { return m_items.value(id); }

    void rebuild()
    {
        const int keep = m_selected;
        for (auto* it : std::as_const(m_items)) {
            removeItem(it);
            delete it;
        }
        m_items.clear();
        for (const auto& n : m_graph.nodes) {
            auto* it = new NodeItem(this, n.id);
            it->setPos(n.x, n.y);
            addItem(it);
            m_items.insert(n.id, it);
        }
        m_selectedEdge.reset();
        setSceneRect(nodesRect().adjusted(-1500, -1000, 1500, 1000));
        select(m_items.contains(keep) ? keep : 0);
        update();
    }

    void select(int id)
    {
        m_selected = id;
        if (id)
            m_selectedEdge.reset();
        for (auto* it : m_items)
            it->update();
        if (onSelected)
            onSelected(id);
    }
    int selected() const { return m_selected; }

    void nodeMoved(int id, QPointF pos)
    {
        if (auto* n = m_graph.find(id)) {
            n->x = pos.x();
            n->y = pos.y();
        }
        update();
    }

    // Texto e cor de cada bloco.
    struct Look {
        QString title, subtitle, icon;
        QColor color;
        bool problem = false;
    };
    Look look(const flow::Node& n) const
    {
        const auto& t = tk();
        switch (n.kind) {
        case NodeKind::Trigger:
            return {triggerTitle(n.trigger), triggerSubtitle(n.trigger), QStringLiteral("clock"), t.warnFg};
        case NodeKind::Command: {
            if (n.command.run.empty())
                return {QStringLiteral("Comando"), QStringLiteral("defina o comando"), QStringLiteral("terminal"), t.errFg, true};
            QString exe = QString::fromStdString(n.command.run.front());
            exe = exe.mid(std::max(exe.lastIndexOf('\\'), exe.lastIndexOf('/')) + 1);
            return {QStringLiteral("Comando · ") + exe, joinCommand(n.command.run), QStringLiteral("terminal"), t.muted};
        }
        case NodeKind::Source: {
            const QString title = fmt::q(m_job.name);
            QString sub = m_job.type == "files" ? QString::fromStdWString(m_job.files.source)
                                                 : QStringLiteral("banco · ") + fmt::q(m_job.database.engine);
            if (m_job.type == "files" && !m_job.files.moreSources.empty())
                sub += QStringLiteral(" + %1 pasta(s)").arg(m_job.files.moreSources.size());
            return {title, sub, m_job.type == "files" ? QStringLiteral("folder") : QStringLiteral("database"), t.accent};
        }
        case NodeKind::Vault: {
            const auto* v = AppContext::instance().config().global.findVault(n.vaultId);
            if (!v)
                return {fmt::q(n.vaultId), QStringLiteral("cofre não declarado"), QStringLiteral("disk"), t.errFg, true};
            QString sub = v->network ? QStringLiteral("em rede") : v->rotation ? QStringLiteral("rodízio") : QStringLiteral("fixo");
            for (const auto& obs : AppContext::instance().vaults())
                if (obs.id == n.vaultId)
                    sub += obs.connected ? QStringLiteral(" · conectado") : QStringLiteral(" · ausente");
            return {fmt::q(v->label), sub, QStringLiteral("disk"), t.okFg};
        }
        }
        return {};
    }

    static QColor portColor(Port p)
    {
        return p == Port::OnSuccess ? tk().okFg : p == Port::OnFailure ? tk().errFg : tk().accent;
    }

protected:
    void drawBackground(QPainter* p, const QRectF& rect) override
    {
        p->fillRect(rect, tk().bg);
        QColor dot = tk().borderStrong;
        p->setPen(QPen(dot, 1.6, Qt::SolidLine, Qt::RoundCap));
        const int step = 24;
        const qreal left = std::floor(rect.left() / step) * step, top = std::floor(rect.top() / step) * step;
        if (rect.width() / step * rect.height() / step < 40000) // grade só em escala legível
            for (qreal x = left; x < rect.right(); x += step)
                for (qreal y = top; y < rect.bottom(); y += step)
                    p->drawPoint(QPointF(x, y));
        // As ligações ficam por baixo dos blocos: desenhadas no fundo, depois da grade.
        drawEdges(p);
    }

    void drawEdges(QPainter* p)
    {
        p->setRenderHint(QPainter::Antialiasing);
        for (const auto& e : m_graph.edges) {
            auto* a = item(e.from);
            auto* b = item(e.to);
            if (!a || !b)
                continue;
            const QPainterPath path = edgePath(a->mapToScene(a->outputPoint(e.port)), b->mapToScene(b->inputPoint()));
            const QColor c = portColor(e.port);
            const bool sel = m_selectedEdge && *m_selectedEdge == e;
            QColor halo = c;
            halo.setAlphaF(sel ? 0.35 : 0.14);
            p->setBrush(Qt::NoBrush);
            p->setPen(QPen(halo, sel ? 10 : 6, Qt::SolidLine, Qt::RoundCap));
            p->drawPath(path);
            p->setPen(QPen(c, 2, Qt::SolidLine, Qt::RoundCap));
            p->drawPath(path);
            p->setPen(Qt::NoPen);
            for (int i = 0; i < 2; ++i) {
                const QPointF pt = path.pointAtPercent(std::fmod(m_phase + i * 0.5, 1.0));
                QColor g = c;
                g.setAlphaF(0.25);
                p->setBrush(g);
                p->drawEllipse(pt, 6.5, 6.5);
                p->setBrush(c);
                p->drawEllipse(pt, 3.2, 3.2);
            }
        }
        if (m_dragging) {
            QPen pen(portColor(m_dragPort), 2, Qt::DashLine, Qt::RoundCap);
            p->setPen(pen);
            p->setBrush(Qt::NoBrush);
            p->drawPath(edgePath(m_dragStart, m_dragEnd));
        }
    }

    void mousePressEvent(QGraphicsSceneMouseEvent* ev) override
    {
        if (ev->button() == Qt::LeftButton) {
            for (auto* it : items(ev->scenePos())) {
                auto* node = dynamic_cast<NodeItem*>(it);
                if (!node)
                    continue;
                if (auto port = node->outputAt(node->mapFromScene(ev->scenePos()))) {
                    m_dragging = true;
                    m_dragFrom = node->id();
                    m_dragPort = *port;
                    m_dragStart = node->mapToScene(node->outputPoint(*port));
                    m_dragEnd = ev->scenePos();
                    ev->accept();
                    return;
                }
                select(node->id());
                QGraphicsScene::mousePressEvent(ev);
                return;
            }
            if (auto e = edgeAt(ev->scenePos())) {
                select(0);
                m_selectedEdge = *e;
                update();
                ev->accept();
                return;
            }
            select(0);
            m_selectedEdge.reset();
            update();
        }
        QGraphicsScene::mousePressEvent(ev);
    }

    void mouseMoveEvent(QGraphicsSceneMouseEvent* ev) override
    {
        if (m_dragging) {
            m_dragEnd = ev->scenePos();
            update();
            return;
        }
        QGraphicsScene::mouseMoveEvent(ev);
    }

    void mouseReleaseEvent(QGraphicsSceneMouseEvent* ev) override
    {
        if (!m_dragging) {
            QGraphicsScene::mouseReleaseEvent(ev);
            return;
        }
        m_dragging = false;
        for (auto* it : items(ev->scenePos())) {
            auto* node = dynamic_cast<NodeItem*>(it);
            if (!node || node->id() == m_dragFrom)
                continue;
            std::string why;
            if (flow::link(m_graph, m_dragFrom, m_dragPort, node->id(), &why)) {
                if (onChanged)
                    onChanged();
            } else if (onMessage) {
                onMessage(QStringLiteral("Não dá para ligar: ") + fmt::q(why) + '.');
            }
            break;
        }
        update();
    }

    void contextMenuEvent(QGraphicsSceneContextMenuEvent* ev) override
    {
        QMenu menu;
        for (auto* it : items(ev->scenePos())) {
            if (auto* node = dynamic_cast<NodeItem*>(it)) {
                const auto* n = m_graph.find(node->id());
                if (!n || n->kind == NodeKind::Source)
                    return;
                select(node->id());
                menu.addAction(QStringLiteral("Remover bloco"), [this, id = node->id()] { removeSelected(id); });
                menu.exec(ev->screenPos());
                return;
            }
        }
        if (auto e = edgeAt(ev->scenePos())) {
            m_selectedEdge = *e;
            update();
            menu.addAction(QStringLiteral("Desligar"), [this] { removeSelected(0); });
            menu.exec(ev->screenPos());
        }
    }

    void keyPressEvent(QKeyEvent* ev) override
    {
        if (ev->key() == Qt::Key_Delete || ev->key() == Qt::Key_Backspace) {
            if (focusItem() == nullptr) {
                removeSelected(m_selected);
                ev->accept();
                return;
            }
        }
        QGraphicsScene::keyPressEvent(ev);
    }

public:
    void removeSelected(int nodeId)
    {
        if (nodeId) {
            const auto* n = m_graph.find(nodeId);
            if (n && n->kind == NodeKind::Source) {
                if (onMessage)
                    onMessage(QStringLiteral("A origem é o centro da tarefa e não pode ser removida."));
                return;
            }
            m_graph.removeNode(nodeId);
            m_selected = 0;
        } else if (m_selectedEdge) {
            m_graph.removeEdge(*m_selectedEdge);
            m_selectedEdge.reset();
        } else {
            return;
        }
        if (onChanged)
            onChanged();
    }

    static QPainterPath edgePath(QPointF a, QPointF b)
    {
        QPainterPath path(a);
        const double bend = std::max(50.0, std::abs(b.x() - a.x()) * 0.5);
        path.cubicTo(a + QPointF(bend, 0), b - QPointF(bend, 0), b);
        return path;
    }

    // Área dos blocos (para enquadrar e para o tamanho da cena).
    QRectF nodesRect() const
    {
        QRectF r;
        for (auto* it : m_items)
            r |= it->sceneBoundingRect();
        return r;
    }

private:
    std::optional<flow::Edge> edgeAt(QPointF pos) const
    {
        QPainterPathStroker stroker;
        stroker.setWidth(12);
        for (const auto& e : m_graph.edges) {
            auto* a = item(e.from);
            auto* b = item(e.to);
            if (a && b && stroker.createStroke(edgePath(a->mapToScene(a->outputPoint(e.port)), b->mapToScene(b->inputPoint()))).contains(pos))
                return e;
        }
        return std::nullopt;
    }

    flow::Graph& m_graph;
    const JobConfig& m_job;
    QHash<int, NodeItem*> m_items;
    int m_selected = 0;
    std::optional<flow::Edge> m_selectedEdge;
    QTimer m_timer;
    double m_phase = 0;
    bool m_dragging = false;
    int m_dragFrom = 0;
    Port m_dragPort = Port::Out;
    QPointF m_dragStart, m_dragEnd;
};

NodeItem::NodeItem(FlowScene* scene, int id) : m_flow(scene), m_id(id)
{
    setFlags(ItemIsMovable | ItemSendsGeometryChanges);
    setCursor(Qt::OpenHandCursor);
    setAcceptHoverEvents(true);
}

double NodeItem::height() const
{
    const auto* n = m_flow->graph().find(m_id);
    return n && n->kind == NodeKind::Source ? kSourceH : kNodeH;
}

bool NodeItem::hasInput() const
{
    const auto* n = m_flow->graph().find(m_id);
    return n && n->kind != NodeKind::Trigger;
}

std::vector<Port> NodeItem::outputs() const
{
    const auto* n = m_flow->graph().find(m_id);
    if (!n || n->kind == NodeKind::Vault)
        return {};
    if (n->kind == NodeKind::Source)
        return {Port::Out, Port::OnSuccess, Port::OnFailure};
    return {Port::Out};
}

QPointF NodeItem::outputPoint(Port port) const
{
    const auto* n = m_flow->graph().find(m_id);
    if (n && n->kind == NodeKind::Source) {
        const double h = height();
        return QPointF(kNodeW, port == Port::Out ? h * 0.30 : port == Port::OnSuccess ? h * 0.58 : h * 0.84);
    }
    return QPointF(kNodeW, height() / 2);
}

std::optional<Port> NodeItem::outputAt(QPointF local) const
{
    for (Port p : outputs()) {
        const QPointF d = local - outputPoint(p);
        if (d.x() * d.x() + d.y() * d.y() <= (kPortR + 5) * (kPortR + 5))
            return p;
    }
    return std::nullopt;
}

QVariant NodeItem::itemChange(GraphicsItemChange change, const QVariant& value)
{
    if (change == ItemPositionHasChanged)
        m_flow->nodeMoved(m_id, value.toPointF());
    return QGraphicsItem::itemChange(change, value);
}

void NodeItem::paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*)
{
    const auto* n = m_flow->graph().find(m_id);
    if (!n)
        return;
    const auto& t = tk();
    const auto look = m_flow->look(*n);
    const double h = height();
    const bool sel = m_flow->selected() == m_id;
    p->setRenderHint(QPainter::Antialiasing);

    const QRectF r(0, 0, kNodeW, h);
    p->setPen(QPen(sel ? t.accent : look.problem ? t.errFg : t.borderStrong, sel ? 2.2 : 1.2));
    p->setBrush(t.surface);
    p->drawRoundedRect(r, 10, 10);

    // Faixa colorida à esquerda e chip do ícone.
    QPainterPath stripe;
    stripe.addRoundedRect(QRectF(0, 0, 10, h), 10, 10);
    QPainterPath clip;
    clip.addRect(QRectF(0, 0, 5, h));
    p->setPen(Qt::NoPen);
    p->setBrush(look.color);
    p->drawPath(stripe.intersected(clip));
    QColor chip = look.color;
    chip.setAlphaF(0.14);
    const QRectF chipRect(14, 14, 36, 36);
    p->setBrush(chip);
    p->drawRoundedRect(chipRect, 8, 8);
    p->drawPixmap(chipRect.adjusted(8, 8, -8, -8).toRect(), icons::pixmap(look.icon, look.color, 40));

    // Textos.
    const double textX = 60;
    const double textW = kNodeW - textX - (n->kind == NodeKind::Source ? 58 : 14);
    QFont f = p->font();
    f.setBold(true);
    f.setPointSizeF(9.5);
    p->setFont(f);
    p->setPen(t.text);
    p->drawText(QRectF(textX, 14, textW, 20), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(f).elidedText(look.title, Qt::ElideRight, int(textW)));
    f.setBold(false);
    f.setPointSizeF(8.2);
    p->setFont(f);
    p->setPen(look.problem ? t.errFg : t.muted);
    p->drawText(QRectF(textX, 34, textW, 18), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(f).elidedText(look.subtitle, Qt::ElideMiddle, int(textW)));
    if (n->kind == NodeKind::Source) {
        p->setPen(t.muted);
        p->drawText(QRectF(14, 60, kNodeW - 28, 18), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("ORIGEM DA CÓPIA"));
        const char* names[] = {"cofres", "sucesso", "falha"};
        int i = 0;
        for (Port port : outputs()) {
            const QPointF pt = outputPoint(port);
            p->setPen(FlowScene::portColor(port));
            p->drawText(QRectF(kNodeW - 70, pt.y() - 9, 58, 18), Qt::AlignRight | Qt::AlignVCenter, QString::fromUtf8(names[i++]));
        }
    }

    // Portas.
    auto port = [&](QPointF c, const QColor& color) {
        p->setPen(QPen(color, 2));
        p->setBrush(t.surface);
        p->drawEllipse(c, kPortR, kPortR);
        p->setPen(Qt::NoPen);
        p->setBrush(color);
        p->drawEllipse(c, kPortR - 3, kPortR - 3);
    };
    if (hasInput())
        port(inputPoint(), t.borderStrong.darker(130));
    for (Port o : outputs())
        port(outputPoint(o), FlowScene::portColor(o));
}

// ============================================================================
// Vista (zoom, arrasto do quadro, receber blocos da paleta) e paleta
// ============================================================================

class FlowView : public QGraphicsView {
public:
    explicit FlowView(FlowScene* scene) : QGraphicsView(scene)
    {
        setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
        setDragMode(QGraphicsView::ScrollHandDrag);
        setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setAcceptDrops(true);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setMinimumSize(520, 420);
    }

    std::function<void(const QString&, QPointF)> onDrop;

    void fit()
    {
        QRectF r = static_cast<FlowScene*>(scene())->nodesRect().adjusted(-60, -60, 60, 60);
        if (r.isEmpty())
            return;
        resetTransform();
        fitInView(r, Qt::KeepAspectRatio);
        const double s = std::clamp(transform().m11(), 0.5, 1.35);
        resetTransform();
        scale(s, s);
        centerOn(r.center());
        m_userZoomed = false;
    }

protected:
    void resizeEvent(QResizeEvent* ev) override
    {
        QGraphicsView::resizeEvent(ev);
        if (!m_userZoomed)
            fit();
    }
    void wheelEvent(QWheelEvent* ev) override
    {
        m_userZoomed = true;
        const double factor = ev->angleDelta().y() > 0 ? 1.12 : 1 / 1.12;
        const double next = transform().m11() * factor;
        if (next < 0.3 || next > 2.0)
            return;
        scale(factor, factor);
    }
    void dragEnterEvent(QDragEnterEvent* ev) override
    {
        if (ev->mimeData()->hasFormat(kMime))
            ev->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent* ev) override
    {
        if (ev->mimeData()->hasFormat(kMime))
            ev->acceptProposedAction();
    }
    void dropEvent(QDropEvent* ev) override
    {
        if (!ev->mimeData()->hasFormat(kMime))
            return;
        if (onDrop)
            onDrop(QString::fromUtf8(ev->mimeData()->data(kMime)), mapToScene(ev->position().toPoint()));
        ev->acceptProposedAction();
    }

private:
    bool m_userZoomed = false;
};

class Palette : public QListWidget {
public:
    Palette()
    {
        setDragEnabled(true);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setIconSize(QSize(20, 20));
        setSpacing(2);
        setFixedWidth(210);
        setFocusPolicy(Qt::NoFocus);
    }
    void addSection(const QString& title)
    {
        auto* it = new QListWidgetItem(title, this);
        it->setFlags(Qt::NoItemFlags);
        QFont f = font();
        f.setBold(true);
        f.setPointSizeF(8);
        it->setFont(f);
        it->setForeground(tk().muted);
    }
    void addBlock(const QString& key, const QString& text, const QString& icon, const QColor& color)
    {
        auto* it = new QListWidgetItem(icons::icon(icon, color, 20), text, this);
        it->setData(Qt::UserRole, key);
        it->setToolTip(QStringLiteral("Arraste para o quadro (ou clique duas vezes)"));
    }

protected:
    void startDrag(Qt::DropActions) override
    {
        auto* it = currentItem();
        if (!it || it->data(Qt::UserRole).toString().isEmpty())
            return;
        auto* mime = new QMimeData;
        mime->setData(kMime, it->data(Qt::UserRole).toString().toUtf8());
        auto* drag = new QDrag(this);
        drag->setMimeData(mime);
        drag->setPixmap(it->icon().pixmap(28, 28));
        drag->exec(Qt::CopyAction);
    }
};

// ============================================================================
// FlowEditorDialog
// ============================================================================

FlowEditorDialog::FlowEditorDialog(const JobConfig& job, QWidget* parent) : QDialog(parent), m_base(job)
{
    setWindowTitle(QStringLiteral("Modo fácil — ") + fmt::q(job.name));
    resize(1380, 820);
    m_graph = flow::fromJob(job, AppContext::instance().config().global);

    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(18, 16, 18, 16);
    v->setSpacing(10);
    v->addWidget(ui::label(QStringLiteral("Puxe blocos da esquerda para o quadro. Para ligar, arraste da bolinha de saída de um bloco "
                                          "até outro. Clique numa linha e tecle Delete para desligar."),
                           "muted"));

    m_scene = new FlowScene(m_graph, m_base, this);
    m_view = new FlowView(m_scene);

    auto* palette = new Palette;
    palette->addSection(QStringLiteral("QUANDO RODAR"));
    const QColor trig = tk().warnFg;
    palette->addBlock(QStringLiteral("daily"), QStringLiteral("Todo dia"), QStringLiteral("clock"), trig);
    palette->addBlock(QStringLiteral("weekly"), QStringLiteral("Dias da semana"), QStringLiteral("clock"), trig);
    palette->addBlock(QStringLiteral("monthly"), QStringLiteral("Uma vez por mês"), QStringLiteral("clock"), trig);
    palette->addBlock(QStringLiteral("every"), QStringLiteral("A cada intervalo"), QStringLiteral("refresh"), trig);
    palette->addBlock(QStringLiteral("startup"), QStringLiteral("Ao ligar o PC"), QStringLiteral("monitor"), trig);
    palette->addBlock(QStringLiteral("logon"), QStringLiteral("Ao entrar no Windows"), QStringLiteral("monitor"), trig);
    palette->addSection(QStringLiteral("AÇÕES"));
    palette->addBlock(QStringLiteral("command"), QStringLiteral("Executar comando"), QStringLiteral("terminal"), tk().muted);
    palette->addSection(QStringLiteral("COFRES"));
    auto* vaultHint = new QListWidgetItem(QStringLiteral("Todos os cofres já estão no\nquadro: ligue a origem a eles."), palette);
    vaultHint->setFlags(Qt::NoItemFlags);
    vaultHint->setForeground(tk().muted);
    connect(palette, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        const QString key = it->data(Qt::UserRole).toString();
        if (!key.isEmpty())
            addBlock(key, m_view->mapToScene(m_view->viewport()->rect().center()));
    });
    m_view->onDrop = [this](const QString& key, QPointF pos) { addBlock(key, pos); };

    buildInspector();

    auto* split = new QSplitter;
    split->addWidget(palette);
    auto* canvasBox = new QWidget;
    auto* cv = new QVBoxLayout(canvasBox);
    cv->setContentsMargins(0, 0, 0, 0);
    cv->setSpacing(6);
    auto* tools = new QHBoxLayout;
    auto* fitBtn = ui::button(QStringLiteral("Enquadrar"), QStringLiteral("zoom-fit"));
    connect(fitBtn, &QPushButton::clicked, this, [this] { m_view->fit(); });
    auto* removeBtn = ui::button(QStringLiteral("Remover selecionado"), QStringLiteral("trash"));
    connect(removeBtn, &QPushButton::clicked, this, [this] { m_scene->removeSelected(m_scene->selected()); });
    tools->addWidget(fitBtn);
    tools->addWidget(removeBtn);
    tools->addStretch(1);
    cv->addLayout(tools);
    cv->addWidget(m_view, 1);
    split->addWidget(canvasBox);
    split->addWidget(m_inspector);
    split->setStretchFactor(1, 1);
    v->addWidget(split, 1);

    m_status = ui::label(QString(), "muted");
    m_status->setWordWrap(true);
    v->addWidget(m_status);

    auto* row = new QHBoxLayout;
    auto* yaml = ui::button(QStringLiteral("Abrir no editor YAML"), QStringLiteral("edit"));
    connect(yaml, &QPushButton::clicked, this, [this] { openYaml(); });
    row->addWidget(yaml);
    row->addStretch(1);
    auto* cancel = ui::button(QStringLiteral("Cancelar"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    m_save = ui::button(QStringLiteral("Salvar"), QStringLiteral("check"), true);
    connect(m_save, &QPushButton::clicked, this, [this] { save(); });
    row->addWidget(cancel);
    row->addWidget(m_save);
    v->addLayout(row);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(250);
    connect(m_debounce, &QTimer::timeout, this, [this] { validate(); });

    m_scene->onChanged = [this] {
        m_scene->rebuild();
        changed();
    };
    m_scene->onSelected = [this](int id) { showInspector(id); };
    m_scene->onMessage = [this](const QString& msg) { m_status->setText(msg); };
    m_scene->rebuild();
    validate();
    QTimer::singleShot(0, this, [this] { m_view->fit(); });
}

void FlowEditorDialog::buildInspector()
{
    m_inspector = new QStackedWidget;
    m_inspector->setFixedWidth(330);

    auto page = [this](const QString& title, const QString& hint) {
        auto* w = new QWidget;
        auto* v = new QVBoxLayout(w);
        v->setContentsMargins(14, 4, 4, 4);
        v->setSpacing(10);
        v->addWidget(ui::label(title, "caption"));
        if (!hint.isEmpty()) {
            auto* h = ui::label(hint, "muted");
            h->setWordWrap(true);
            v->addWidget(h);
        }
        m_inspector->addWidget(w);
        return v;
    };

    // 0: nada selecionado
    {
        auto* v = page(QStringLiteral("COMO FUNCIONA"),
                       QStringLiteral("• Gatilhos (laranja) dizem quando a tarefa roda: ligue-os à origem.\n\n"
                                      "• Comandos ligados antes da origem rodam antes da cópia (ex.: parar o sistema do PDV); "
                                      "os ligados às saídas rodam depois: sempre, só no sucesso ou só na falha.\n\n"
                                      "• Cofres (verde) recebem a cópia: ligue a saída \"cofres\" da origem a cada disco.\n\n"
                                      "• Selecione um bloco para ajustá-lo aqui."));
        v->addStretch(1);
    }
    // 1: gatilho
    {
        auto* v = page(QStringLiteral("GATILHO"), QString());
        m_trForm = new QFormLayout;
        m_trKind = new QComboBox;
        for (const auto& [text, kind] : std::vector<std::pair<QString, TriggerKind>>{
                 {QStringLiteral("Todo dia"), TriggerKind::Daily},
                 {QStringLiteral("Dias da semana"), TriggerKind::Weekly},
                 {QStringLiteral("Uma vez por mês"), TriggerKind::Monthly},
                 {QStringLiteral("A cada intervalo"), TriggerKind::Every},
                 {QStringLiteral("Ao ligar o PC"), TriggerKind::OnStartup},
                 {QStringLiteral("Ao entrar no Windows"), TriggerKind::OnLogon}})
            m_trKind->addItem(text, int(kind));
        m_trTime = new QTimeEdit;
        m_trTime->setDisplayFormat(QStringLiteral("HH:mm"));
        m_trDaysBox = new QWidget;
        auto* dl = new QHBoxLayout(m_trDaysBox);
        dl->setContentsMargins(0, 0, 0, 0);
        dl->setSpacing(2);
        for (int i = 0; i < 7; ++i) {
            m_trDays[i] = new QCheckBox(weekdayShort(i + 1));
            dl->addWidget(m_trDays[i]);
            connect(m_trDays[i], &QCheckBox::toggled, this, [this] { storeTrigger(); });
        }
        m_trDayOfMonth = new QSpinBox;
        m_trDayOfMonth->setRange(0, 28);
        m_trDayOfMonth->setSpecialValueText(QStringLiteral("último dia"));
        m_trInterval = new QSpinBox;
        m_trInterval->setRange(5, 24 * 60);
        m_trInterval->setSuffix(QStringLiteral(" min"));
        m_trDelay = new QSpinBox;
        m_trDelay->setRange(0, 240);
        m_trDelay->setSuffix(QStringLiteral(" min depois"));
        m_trForm->addRow(QStringLiteral("Tipo"), m_trKind);
        m_trForm->addRow(QStringLiteral("Horário"), m_trTime);
        m_trForm->addRow(QStringLiteral("Dias"), m_trDaysBox);
        m_trForm->addRow(QStringLiteral("Dia do mês"), m_trDayOfMonth);
        m_trForm->addRow(QStringLiteral("Intervalo"), m_trInterval);
        m_trForm->addRow(QStringLiteral("Atraso"), m_trDelay);
        v->addLayout(m_trForm);
        m_trSplit = ui::button(QStringLiteral("Horário diferente em um dia…"), QStringLiteral("clock"));
        m_trSplit->setToolTip(QStringLiteral("Tira um dia deste gatilho e cria outro só para ele, com o horário escolhido "
                                             "(ex.: domingo ao meio-dia)."));
        connect(m_trSplit, &QPushButton::clicked, this, [this] { splitTriggerDay(); });
        v->addWidget(m_trSplit, 0, Qt::AlignLeft);
        v->addStretch(1);
        connect(m_trKind, &QComboBox::currentIndexChanged, this, [this] { storeTrigger(); });
        connect(m_trTime, &QTimeEdit::timeChanged, this, [this] { storeTrigger(); });
        for (auto* s : {m_trDayOfMonth, m_trInterval, m_trDelay})
            connect(s, &QSpinBox::valueChanged, this, [this] { storeTrigger(); });
    }
    // 2: comando
    {
        auto* v = page(QStringLiteral("COMANDO"),
                       QStringLiteral("Programa e argumentos, como no Prompt de Comando. Use aspas em caminhos com espaço. "
                                      "Senhas não devem ir aqui: use segredos."));
        m_cmdRole = ui::label(QString(), "fact");
        m_cmdRole->setWordWrap(true);
        v->addWidget(m_cmdRole);
        auto* f = new QFormLayout;
        m_cmdLine = new QLineEdit;
        m_cmdLine->setPlaceholderText(QStringLiteral("ex.: \"C:\\PDV\\parar.cmd\" /silencioso"));
        m_cmdTimeout = new QSpinBox;
        m_cmdTimeout->setRange(5, 4 * 3600);
        m_cmdTimeout->setSuffix(QStringLiteral(" s"));
        m_cmdOnError = new QComboBox;
        m_cmdOnError->addItem(QStringLiteral("Interromper a tarefa"), true);
        m_cmdOnError->addItem(QStringLiteral("Só registrar um aviso"), false);
        f->addRow(QStringLiteral("Comando"), m_cmdLine);
        f->addRow(QStringLiteral("Tempo limite"), m_cmdTimeout);
        f->addRow(QStringLiteral("Se falhar"), m_cmdOnError);
        v->addLayout(f);
        v->addStretch(1);
        connect(m_cmdLine, &QLineEdit::textChanged, this, [this] { storeCommand(); });
        connect(m_cmdTimeout, &QSpinBox::valueChanged, this, [this] { storeCommand(); });
        connect(m_cmdOnError, &QComboBox::currentIndexChanged, this, [this] { storeCommand(); });
    }
    // 3: origem
    {
        auto* v = page(QStringLiteral("ORIGEM"), QString());
        auto* f = new QFormLayout;
        m_srcName = new QLineEdit;
        f->addRow(QStringLiteral("Nome da tarefa"), m_srcName);
        v->addLayout(f);
        m_srcFilesBox = new QWidget;
        auto* ff = new QVBoxLayout(m_srcFilesBox);
        ff->setContentsMargins(0, 0, 0, 0);
        ff->addWidget(ui::label(QStringLiteral("Pastas"), "fact"));
        m_srcList = new SourceListEditor;
        connect(m_srcList, &SourceListEditor::changed, this, [this] { storeSource(); });
        ff->addWidget(m_srcList);
        ff->addWidget(ui::label(QStringLiteral("Excluir"), "fact"));
        m_srcExclude = new QLineEdit;
        m_srcExclude->setPlaceholderText(QStringLiteral("**/*.tmp, Logs/**"));
        ff->addWidget(m_srcExclude);
        v->addWidget(m_srcFilesBox);
        m_srcDbInfo = ui::label(QString(), "muted");
        m_srcDbInfo->setWordWrap(true);
        v->addWidget(m_srcDbInfo);
        m_srcShutdown = new QCheckBox(QStringLiteral("Desligar o computador depois do backup"));
        v->addWidget(m_srcShutdown);
        auto* more = ui::label(QStringLiteral("Retenção, verificação e proteções continuam como estão no arquivo; "
                                              "para mudá-las, use o editor YAML."),
                               "muted");
        more->setWordWrap(true);
        v->addWidget(more);
        v->addStretch(1);
        for (auto* e : {m_srcName, m_srcExclude})
            connect(e, &QLineEdit::textChanged, this, [this] { storeSource(); });
        connect(m_srcShutdown, &QCheckBox::toggled, this, [this] { storeSource(); });
    }
    // 4: cofre
    {
        auto* v = page(QStringLiteral("COFRE"), QString());
        m_vaultInfo = ui::label(QString(), "fact");
        m_vaultInfo->setWordWrap(true);
        m_vaultInfo->setTextFormat(Qt::RichText);
        v->addWidget(m_vaultInfo);
        auto* h = ui::label(QStringLiteral("Ligado à saída \"cofres\" da origem, o disco recebe a cópia. Para cadastrar outro "
                                           "disco ou mudar rodízio e limites, use a tela Cofres."),
                            "muted");
        h->setWordWrap(true);
        v->addWidget(h);
        v->addStretch(1);
    }
}

void FlowEditorDialog::showInspector(int nodeId)
{
    m_current = nodeId;
    const auto* n = m_graph.find(nodeId);
    if (!n) {
        m_inspector->setCurrentIndex(0);
        return;
    }
    m_loading = true;
    switch (n->kind) {
    case NodeKind::Trigger: loadTrigger(*n); m_inspector->setCurrentIndex(1); break;
    case NodeKind::Command: loadCommand(*n); m_inspector->setCurrentIndex(2); break;
    case NodeKind::Source: loadSource(); m_inspector->setCurrentIndex(3); break;
    case NodeKind::Vault: loadVault(*n); m_inspector->setCurrentIndex(4); break;
    }
    m_loading = false;
}

void FlowEditorDialog::loadTrigger(const flow::Node& n)
{
    const auto& t = n.trigger;
    m_trKind->setCurrentIndex(std::max(0, m_trKind->findData(int(t.kind))));
    m_trTime->setTime(QTime(t.atMinutes / 60, t.atMinutes % 60));
    for (int i = 0; i < 7; ++i)
        m_trDays[i]->setChecked(std::find(t.days.begin(), t.days.end(), i + 1) != t.days.end());
    m_trDayOfMonth->setValue(t.dayOfMonth < 0 ? 0 : t.dayOfMonth);
    m_trInterval->setValue(int(std::max<std::int64_t>(5, t.intervalSeconds / 60)));
    m_trDelay->setValue(int(t.delaySeconds / 60));
    const auto k = t.kind;
    m_trForm->setRowVisible(m_trTime, k == TriggerKind::Daily || k == TriggerKind::Weekly || k == TriggerKind::Monthly);
    m_trForm->setRowVisible(m_trDaysBox, k == TriggerKind::Weekly || k == TriggerKind::Every);
    m_trForm->setRowVisible(m_trDayOfMonth, k == TriggerKind::Monthly);
    m_trForm->setRowVisible(m_trInterval, k == TriggerKind::Every);
    m_trForm->setRowVisible(m_trDelay, k == TriggerKind::OnStartup || k == TriggerKind::OnLogon);
    m_trSplit->setVisible(k == TriggerKind::Daily || (k == TriggerKind::Weekly && t.days.size() > 1));
}

void FlowEditorDialog::splitTriggerDay()
{
    auto* n = m_graph.find(m_current);
    if (!n || n->kind != NodeKind::Trigger)
        return;
    const ScheduleTrigger original = n->trigger;
    std::vector<int> days = original.kind == TriggerKind::Daily ? std::vector<int>{1, 2, 3, 4, 5, 6, 7} : original.days;
    if (days.size() < 2)
        return;

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Horário diferente em um dia"));
    auto* f = new QFormLayout(&dlg);
    f->setContentsMargins(20, 18, 20, 18);
    auto* day = new QComboBox;
    static const char* longNames[] = {"Segunda", "Terça", "Quarta", "Quinta", "Sexta", "Sábado", "Domingo"};
    for (int d : days)
        day->addItem(QString::fromUtf8(longNames[d - 1]), d);
    day->setCurrentIndex(day->count() - 1); // domingo, o caso mais comum
    auto* time = new QTimeEdit(QTime(12, 0));
    time->setDisplayFormat(QStringLiteral("HH:mm"));
    f->addRow(QStringLiteral("Dia"), day);
    f->addRow(QStringLiteral("Horário"), time);
    auto* hint = ui::label(QStringLiteral("O dia sai deste gatilho e ganha um gatilho próprio, ligado ao mesmo destino."), "muted");
    hint->setWordWrap(true);
    f->addRow(hint);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    box->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Criar"));
    box->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Cancelar"));
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    f->addRow(box);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const int chosen = day->currentData().toInt();
    days.erase(std::remove(days.begin(), days.end(), chosen), days.end());
    n->trigger.kind = TriggerKind::Weekly;
    n->trigger.days = days;

    flow::Node extra;
    extra.id = m_graph.nextId();
    extra.kind = NodeKind::Trigger;
    extra.trigger.kind = TriggerKind::Weekly;
    extra.trigger.days = {chosen};
    extra.trigger.atMinutes = time->time().hour() * 60 + time->time().minute();
    extra.x = n->x;
    extra.y = n->y + kNodeH + 24;
    const int originalId = n->id; // `n` deixa de valer depois do push_back
    m_graph.nodes.push_back(extra);
    std::vector<flow::Edge> copies;
    for (const auto& e : m_graph.edges)
        if (e.from == originalId)
            copies.push_back({extra.id, e.port, e.to});
    for (const auto& e : copies)
        m_graph.edges.push_back(e);

    m_scene->rebuild();
    m_scene->select(extra.id);
    changed();
}

void FlowEditorDialog::storeTrigger()
{
    auto* n = m_graph.find(m_current);
    if (m_loading || !n || n->kind != NodeKind::Trigger)
        return;
    auto& t = n->trigger;
    const auto kind = TriggerKind(m_trKind->currentData().toInt());
    const bool kindChanged = kind != t.kind;
    t.kind = kind;
    t.atMinutes = m_trTime->time().hour() * 60 + m_trTime->time().minute();
    t.days.clear();
    for (int i = 0; i < 7; ++i)
        if (m_trDays[i]->isChecked())
            t.days.push_back(i + 1);
    if (kindChanged && kind == TriggerKind::Weekly && t.days.empty())
        t.days = {1, 2, 3, 4, 5};
    if (kind != TriggerKind::Weekly && kind != TriggerKind::Every)
        t.days.clear();
    t.dayOfMonth = kind == TriggerKind::Monthly ? (m_trDayOfMonth->value() == 0 ? -1 : m_trDayOfMonth->value()) : 0;
    t.intervalSeconds = kind == TriggerKind::Every ? std::int64_t(m_trInterval->value()) * 60 : 0;
    t.delaySeconds = (kind == TriggerKind::OnStartup || kind == TriggerKind::OnLogon) ? std::int64_t(m_trDelay->value()) * 60 : 0;
    if (kindChanged) {
        m_loading = true;
        loadTrigger(*n);
        m_loading = false;
    }
    changed();
}

void FlowEditorDialog::loadCommand(const flow::Node& n)
{
    m_cmdLine->setText(joinCommand(n.command.run));
    m_cmdTimeout->setValue(int(n.command.timeoutSeconds));
    m_cmdOnError->setCurrentIndex(n.command.failOnError ? 0 : 1);
    updateCommandRole(n);
}

void FlowEditorDialog::updateCommandRole(const flow::Node& n)
{
    // Papel do comando, conforme as ligações atuais.
    const auto conv = flow::toJob(m_graph, m_base);
    auto contains = [&](const std::vector<HookCommand>& list) {
        int i = 0;
        for (const auto& c : list) {
            ++i;
            if (c.run == n.command.run)
                return i;
        }
        return 0;
    };
    const std::pair<const std::vector<HookCommand>*, QString> roles[] = {
        {&conv.job.hooks.before, QStringLiteral("Roda ANTES da cópia (passo %1).")},
        {&conv.job.hooks.after, QStringLiteral("Roda DEPOIS da cópia, sempre (passo %1).")},
        {&conv.job.hooks.onSuccess, QStringLiteral("Roda depois da cópia, só se der certo (passo %1).")},
        {&conv.job.hooks.onFailure, QStringLiteral("Roda depois da cópia, só se falhar (passo %1).")},
    };
    QString role = QStringLiteral("Solto: ligue-o antes da origem ou a uma das saídas dela.");
    for (const auto& [list, text] : roles)
        if (const int step = contains(*list)) {
            role = text.arg(step);
            break;
        }
    m_cmdRole->setText(role);
}

void FlowEditorDialog::storeCommand()
{
    auto* n = m_graph.find(m_current);
    if (m_loading || !n || n->kind != NodeKind::Command)
        return;
    n->command.run.clear();
    for (const auto& a : QProcess::splitCommand(m_cmdLine->text()))
        n->command.run.push_back(a.toStdString());
    n->command.timeoutSeconds = m_cmdTimeout->value();
    n->command.failOnError = m_cmdOnError->currentData().toBool();
    changed();
}

void FlowEditorDialog::loadSource()
{
    m_srcName->setText(fmt::q(m_base.name));
    const bool files = m_base.type == "files";
    m_srcFilesBox->setVisible(files);
    m_srcDbInfo->setVisible(!files);
    if (files) {
        m_srcList->setSources(m_base.files);
        QStringList ex;
        for (const auto& e : m_base.files.exclude)
            ex << QString::fromStdWString(e);
        m_srcExclude->setText(ex.join(QStringLiteral(", ")));
    } else {
        const auto& d = m_base.database;
        QString info = QStringLiteral("Banco de dados: ") + fmt::q(d.engine);
        if (!d.host.empty())
            info += QStringLiteral(" em ") + fmt::q(d.host);
        info += QStringLiteral(". A conexão e os bancos ficam no editor YAML (ou recrie a tarefa pelo assistente).");
        m_srcDbInfo->setText(info);
    }
    m_srcShutdown->setChecked(m_base.shutdownAfter);
}

void FlowEditorDialog::storeSource()
{
    if (m_loading)
        return;
    m_base.name = m_srcName->text().trimmed().toStdString();
    if (m_base.type == "files") {
        m_srcList->applyTo(m_base.files);
        m_base.files.exclude.clear();
        for (const auto& p : m_srcExclude->text().split(',', Qt::SkipEmptyParts))
            if (!p.trimmed().isEmpty())
                m_base.files.exclude.push_back(p.trimmed().toStdWString());
    }
    m_base.shutdownAfter = m_srcShutdown->isChecked();
    changed();
}

void FlowEditorDialog::loadVault(const flow::Node& n)
{
    const auto& ctx = AppContext::instance();
    const auto* v = ctx.config().global.findVault(n.vaultId);
    if (!v) {
        m_vaultInfo->setText(QStringLiteral("<b>%1</b><br>Este cofre não está declarado em synkro.yml. Desligue-o da origem "
                                            "ou cadastre o disco na tela Cofres.")
                                 .arg(fmt::q(n.vaultId).toHtmlEscaped()));
        return;
    }
    QString state = QStringLiteral("sem contato ainda");
    for (const auto& obs : ctx.vaults())
        if (obs.id == n.vaultId)
            state = obs.connected ? QStringLiteral("conectado") : QStringLiteral("ausente agora");
    QString path = QString::fromStdWString(v->path);
    m_vaultInfo->setText(QStringLiteral("<b>%1</b><br>id: %2<br>%3%4<br>%5")
                             .arg(fmt::q(v->label).toHtmlEscaped(), fmt::q(v->id).toHtmlEscaped(),
                                  v->network ? QStringLiteral("pasta na rede") : v->rotation ? QStringLiteral("disco em rodízio") : QStringLiteral("disco fixo"),
                                  path.isEmpty() ? QString() : QStringLiteral(" · ") + path.toHtmlEscaped(), state));
}

void FlowEditorDialog::addBlock(const QString& type, QPointF scenePos)
{
    flow::Node n;
    n.id = m_graph.nextId();
    if (type == QLatin1String("command")) {
        n.kind = NodeKind::Command;
    } else {
        n.kind = NodeKind::Trigger;
        auto& t = n.trigger;
        if (type == QLatin1String("daily")) {
            t.kind = TriggerKind::Daily;
            t.atMinutes = 22 * 60;
        } else if (type == QLatin1String("weekly")) {
            t.kind = TriggerKind::Weekly;
            t.atMinutes = 22 * 60;
            t.days = {1, 2, 3, 4, 5};
        } else if (type == QLatin1String("monthly")) {
            t.kind = TriggerKind::Monthly;
            t.atMinutes = 3 * 60;
            t.dayOfMonth = 1;
        } else if (type == QLatin1String("every")) {
            t.kind = TriggerKind::Every;
            t.intervalSeconds = 3600;
        } else if (type == QLatin1String("startup")) {
            t.kind = TriggerKind::OnStartup;
            t.delaySeconds = 600;
        } else {
            t.kind = TriggerKind::OnLogon;
            t.delaySeconds = 300;
        }
    }
    n.x = scenePos.x() - kNodeW / 2;
    n.y = scenePos.y() - kNodeH / 2;
    m_graph.nodes.push_back(n);
    m_scene->rebuild();
    m_scene->select(n.id);
    m_status->setText(n.kind == NodeKind::Trigger
                          ? QStringLiteral("Gatilho criado. Arraste da bolinha à direita dele até a origem para valer.")
                          : QStringLiteral("Comando criado. Ligue-o antes da origem ou a uma das saídas dela e defina o comando."));
    changed();
}

void FlowEditorDialog::changed()
{
    if (auto* it = m_scene->item(m_current))
        it->update();
    m_scene->update();
    m_debounce->start();
}

void FlowEditorDialog::validate()
{
    const auto conv = flow::toJob(m_graph, m_base);
    const std::string yaml = emitJob(conv.job);
    std::vector<Diagnostic> diags;
    const auto& set = AppContext::instance().config();
    (void)parseJobDocument(yaml, m_base.file, set.global, diags);
    QStringList errors, warnings;
    for (const auto& d : diags) {
        QString msg = fmt::q(d.key.empty() ? d.message : d.key + ": " + d.message).toHtmlEscaped();
        (d.severity == Severity::Error ? errors : warnings) << msg;
    }
    for (const auto& w : conv.warnings)
        warnings << fmt::q(w).toHtmlEscaped();
    m_hasErrors = !errors.isEmpty();
    m_save->setEnabled(!m_hasErrors);
    QString text;
    if (!errors.isEmpty())
        text += QStringLiteral("<span style='color:%1'>✗ %2</span><br>").arg(tk().errFg.name(), errors.join(QStringLiteral("<br>✗ ")));
    if (!warnings.isEmpty())
        text += QStringLiteral("<span style='color:%1'>! %2</span>").arg(tk().warnFg.name(), warnings.join(QStringLiteral(" · ")));
    if (text.isEmpty()) {
        QStringList when;
        for (const auto& t : conv.job.schedule)
            when << triggerTitle(t);
        text = QStringLiteral("<span style='color:%1'>✓ Pronto para salvar.</span> %2 · %3 cofre(s) · %4 comando(s)")
                   .arg(tk().okFg.name(), when.isEmpty() ? QStringLiteral("só sob demanda") : when.join(QStringLiteral(", ")).toHtmlEscaped())
                   .arg(conv.job.vaults.size())
                   .arg(conv.job.hooks.before.size() + conv.job.hooks.after.size() + conv.job.hooks.onSuccess.size()
                        + conv.job.hooks.onFailure.size());
    }
    m_status->setTextFormat(Qt::RichText);
    m_status->setText(text);
    if (const auto* n = m_graph.find(m_current); n && n->kind == NodeKind::Command)
        updateCommandRole(*n);
}

void FlowEditorDialog::save()
{
    validate();
    if (m_hasErrors)
        return;
    const auto conv = flow::toJob(m_graph, m_base);
    if (!conv.warnings.empty()) {
        QStringList w;
        for (const auto& s : conv.warnings)
            w << QStringLiteral("• ") + fmt::q(s);
        if (QMessageBox::question(this, windowTitle(), QStringLiteral("Há blocos soltos:\n\n%1\n\nSalvar mesmo assim?").arg(w.join('\n')))
            != QMessageBox::Yes)
            return;
    }
    const std::string yaml = emitJob(conv.job);
    auto& ctx = AppContext::instance();
    const std::string current = ctx.readConfigFile(m_base.file);
    if (!current.empty() && !isPristineGenerated(current)) {
        // Arquivo com edições à mão: o modo fácil regera o YAML inteiro. Mostra
        // o resultado no editor para a pessoa conferir antes de gravar.
        YamlEditorDialog editor(m_base.file, this, QString::fromStdString(yaml),
                                QStringLiteral("Este arquivo tinha edições feitas à mão. O modo fácil gerou o YAML abaixo; "
                                               "comentários e ajustes que ele não conhece não aparecem aqui. Confira e salve."));
        if (editor.exec() == QDialog::Accepted)
            accept();
        return;
    }
    if (auto st = ctx.writeConfigFile(m_base.file, yaml); !st) {
        QMessageBox::warning(this, windowTitle(), fmt::q(st.error().message));
        return;
    }
    accept();
}

void FlowEditorDialog::openYaml()
{
    const auto conv = flow::toJob(m_graph, m_base);
    YamlEditorDialog editor(m_base.file, this, QString::fromStdString(emitJob(conv.job)),
                            QStringLiteral("YAML gerado pelo modo fácil (ainda não salvo). Ajuste o que quiser e salve."));
    if (editor.exec() == QDialog::Accepted)
        accept();
}

void FlowEditorDialog::selectFirst(NodeKind kind)
{
    for (const auto& n : m_graph.nodes)
        if (n.kind == kind) {
            m_scene->select(n.id);
            return;
        }
}
