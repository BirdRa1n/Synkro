#include "Ui.h"

#include "Icons.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLayoutItem>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace ui {

namespace {

std::pair<QColor, QColor> toneColors(Tone tone)
{
    const auto& t = tk();
    switch (tone) {
    case Tone::Ok:      return {t.okBg, t.okFg};
    case Tone::Warn:    return {t.warnBg, t.warnFg};
    case Tone::Error:   return {t.errBg, t.errFg};
    case Tone::Accent:  return {t.chip, t.accent};
    case Tone::Neutral: break;
    }
    return {t.inset, t.muted};
}

} // namespace

QLabel* label(const QString& text, const char* role, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    if (role)
        l->setProperty("role", role);
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}

QFrame* card(const char* objectName)
{
    auto* f = new QFrame;
    f->setObjectName(objectName);
    return f;
}

QPushButton* button(const QString& text, const QString& iconName, bool primary)
{
    auto* b = new QPushButton(text);
    b->setCursor(Qt::PointingHandCursor);
    if (primary)
        b->setProperty("role", "primary");
    if (!iconName.isEmpty()) {
        b->setProperty("iconName", iconName);
        b->setIcon(icons::icon(iconName, primary ? QColor(Qt::white) : tk().text, 18));
    }
    return b;
}

void retintButtons(QWidget* root)
{
    for (auto* b : root->findChildren<QPushButton*>()) {
        const QString name = b->property("iconName").toString();
        if (name.isEmpty())
            continue;
        const bool primary = b->property("role").toString() == QLatin1String("primary");
        b->setIcon(icons::icon(name, primary ? QColor(Qt::white) : tk().text, 18));
    }
}

QLabel* pill(const QString& text, Tone tone)
{
    auto [bg, fg] = toneColors(tone);
    auto* l = new QLabel(text);
    l->setStyleSheet(QStringLiteral("background:%1; color:%2; border-radius:9px; padding:2px 10px; font-size:8.5pt; font-weight:600;")
                         .arg(bg.name(), fg.name()));
    l->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    return l;
}

QLabel* badge(Tone tone)
{
    auto [bg, fg] = toneColors(tone);
    auto* l = new QLabel;
    l->setFixedSize(24, 24);
    l->setAlignment(Qt::AlignCenter);
    const QString name = tone == Tone::Ok ? "check" : tone == Tone::Error ? "x" : "alert";
    l->setPixmap(icons::pixmap(name, fg, 14));
    l->setStyleSheet(QStringLiteral("background:%1; border-radius:12px;").arg(bg.name()));
    return l;
}

QLabel* iconChip(const QString& iconName, int size)
{
    auto* l = new QLabel;
    l->setFixedSize(size, size);
    l->setAlignment(Qt::AlignCenter);
    l->setPixmap(icons::pixmap(iconName, tk().accent, size / 2 + 2));
    l->setStyleSheet(QStringLiteral("background:%1; border-radius:12px;").arg(tk().chip.name()));
    return l;
}

void clearLayout(QLayout* layout)
{
    if (!layout)
        return;
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (QWidget* w = item->widget()) {
            w->hide(); // some na hora; a destruição fica para o laço de eventos
            w->deleteLater();
        }
        else if (QLayout* l = item->layout())
            clearLayout(l);
        delete item;
    }
}

UsageBar::UsageBar(double used, double secondary, QWidget* parent) : QWidget(parent), m_used(used), m_secondary(secondary)
{
    setFixedHeight(10);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void UsageBar::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = rect();
    QPainterPath clip;
    clip.addRoundedRect(r, 5, 5);
    p.setClipPath(clip);
    p.fillRect(r, tk().track);
    const double used = std::clamp(m_used, 0.0, 1.0);
    const double sec = std::clamp(m_secondary, 0.0, 1.0 - used);
    p.fillRect(QRectF(0, 0, r.width() * used, r.height()), tk().accent);
    p.fillRect(QRectF(r.width() * used, 0, r.width() * sec, r.height()), tk().soft);
}

FlowLinks::FlowLinks(QWidget* parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

void FlowLinks::setLinks(std::vector<Link> links)
{
    m_links = std::move(links);
    const bool anyActive = std::any_of(m_links.begin(), m_links.end(), [](const Link& l) { return l.active; });
    if (anyActive && !m_timer)
        m_timer = startTimer(33);
    else if (!anyActive && m_timer) {
        killTimer(m_timer);
        m_timer = 0;
    }
    update();
}

void FlowLinks::timerEvent(QTimerEvent*)
{
    if (!isVisible())
        return; // tela escondida: nada a desenhar
    m_phase += 0.012;
    if (m_phase >= 1.0)
        m_phase -= 1.0;
    update();
}

void FlowLinks::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const double w = width();
    for (const auto& link : m_links) {
        if (!link.from || !link.to || !link.from->isVisible() || !link.to->isVisible())
            continue;
        const QPoint a = mapFromGlobal(link.from->mapToGlobal(QPoint(link.from->width(), link.from->height() / 2)));
        const QPoint b = mapFromGlobal(link.to->mapToGlobal(QPoint(0, link.to->height() / 2)));
        const QPointF start(0, a.y()), end(w - 10, b.y());
        QPainterPath path(start);
        const double bend = std::max(30.0, (end.x() - start.x()) * 0.5);
        path.cubicTo(start + QPointF(bend, 0), end - QPointF(bend, 0), end);

        const QColor color = link.active ? tk().accent : tk().dash;
        p.setBrush(Qt::NoBrush);
        // Trilho de fundo, mais largo e translúcido.
        if (link.active) {
            QColor halo = color;
            halo.setAlphaF(0.15);
            p.setPen(QPen(halo, 6, Qt::SolidLine, Qt::RoundCap));
            p.drawPath(path);
        }
        QPen pen(color, 2, link.active ? Qt::SolidLine : Qt::DashLine, Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);

        // Conectores nas pontas.
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawEllipse(start + QPointF(1, 0), 4, 4);
        QPolygonF head;
        head << QPointF(w - 11, b.y() - 5) << QPointF(w - 2, b.y()) << QPointF(w - 11, b.y() + 5);
        p.drawPolygon(head);

        // Pulsos de dados correndo até o disco.
        if (link.active) {
            constexpr int pulses = 3;
            for (int i = 0; i < pulses; ++i) {
                const double t = std::fmod(m_phase + double(i) / pulses, 1.0);
                const QPointF pt = path.pointAtPercent(t);
                QColor glow = color;
                glow.setAlphaF(0.25);
                p.setBrush(glow);
                p.drawEllipse(pt, 7, 7);
                p.setBrush(color);
                p.drawEllipse(pt, 3.5, 3.5);
            }
        }
    }
}

Page::Page(const QString& title, const QString& subtitle, QWidget* parent) : QWidget(parent)
{
    setObjectName("page");
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll);

    auto* content = new QWidget;
    content->setObjectName("page");
    scroll->setWidget(content);
    auto* v = new QVBoxLayout(content);
    v->setContentsMargins(36, 28, 36, 32);
    v->setSpacing(24);

    auto* header = new QHBoxLayout;
    auto* titles = new QVBoxLayout;
    titles->setSpacing(2);
    titles->addWidget(label(title, "h1"));
    titles->addWidget(label(subtitle, "muted"));
    header->addLayout(titles, 1);
    m_actions = new QHBoxLayout;
    m_actions->setSpacing(10);
    header->addLayout(m_actions);
    header->setAlignment(m_actions, Qt::AlignBottom);
    v->addLayout(header);

    m_body = new QVBoxLayout;
    m_body->setSpacing(24);
    v->addLayout(m_body);
    v->addStretch(1);
}

} // namespace ui
