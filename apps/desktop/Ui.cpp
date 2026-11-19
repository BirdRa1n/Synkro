#include "Ui.h"

#include "Icons.h"
#include "Theme.h"

#include <QHBoxLayout>
#include <QLayoutItem>
#include <QPainter>
#include <QPainterPath>

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

Arrow::Arrow(bool dashed, QWidget* parent) : QWidget(parent), m_dashed(dashed)
{
    setMinimumSize(80, 20);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void Arrow::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor color = m_dashed ? tk().dash : tk().accent;
    QPen pen(color, 2);
    if (m_dashed)
        pen.setStyle(Qt::DashLine);
    p.setPen(pen);
    const int y = height() / 2;
    p.drawLine(4, y, width() - 12, y);
    QPolygonF head;
    head << QPointF(width() - 12, y - 6) << QPointF(width() - 2, y) << QPointF(width() - 12, y + 6);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPolygon(head);
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
