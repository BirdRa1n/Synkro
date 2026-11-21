#include "Icons.h"

#include <QHash>
#include <QPainter>
#include <QSvgRenderer>

namespace icons {

namespace {

const QHash<QString, QString>& paths()
{
    static const QHash<QString, QString> p = {
        {"shield", "M12 3l8 4v5c0 4.5-3.4 8.3-8 9-4.6-.7-8-4.5-8-9V7z"},
        {"shield-check", "M12 3l8 4v5c0 4.5-3.4 8.3-8 9-4.6-.7-8-4.5-8-9V7z M8.5 12l2.5 2.5 4.5-5"},
        {"home", "M3 11l9-7 9 7v9a1 1 0 0 1-1 1h-5v-6H9v6H4a1 1 0 0 1-1-1z"},
        {"jobs", "M8 6h13M8 12h13M8 18h13M3 6h.01M3 12h.01M3 18h.01"},
        {"disk", "M3 12h18M5 12l2-7h10l2 7v6a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1zM16 16h.01"},
        {"history", "M12 8v4l3 2M3 12a9 9 0 1 0 3-6.7L3 8M3 3v5h5"},
        {"restore", "M21 8v13H3V8M1 3h22v5H1zM10 12h4"},
        {"chevron-down", "M6 9l6 6 6-6"},
        {"settings", "M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6"},
        {"sun", "M12 17a5 5 0 1 0 0-10 5 5 0 0 0 0 10zM12 1v2M12 21v2M4.2 4.2l1.4 1.4M18.4 18.4l1.4 1.4M1 12h2M21 12h2M4.2 19.8l1.4-1.4M18.4 5.6l1.4-1.4"},
        {"moon", "M21 12.8A9 9 0 1 1 11.2 3a7 7 0 0 0 9.8 9.8z"},
        {"monitor", "M3 4h18v12H3zM8 20h8M12 16v4"},
        {"database", "M4 6c0-1.7 3.6-3 8-3s8 1.3 8 3-3.6 3-8 3-8-1.3-8-3zm0 0v12c0 1.7 3.6 3 8 3s8-1.3 8-3V6M4 12c0 1.7 3.6 3 8 3s8-1.3 8-3"},
        {"folder", "M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z"},
        {"play", "M7 4l13 8-13 8z"},
        {"plus", "M12 5v14M5 12h14"},
        {"refresh", "M20 12a8 8 0 1 1-2.3-5.6M20 4v5h-5"},
        {"check", "M5 12.5l4.5 4.5L19 7.5"},
        {"alert", "M12 9v4M12 17h.01M10.3 3.9L1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z"},
        {"x", "M7 7l10 10M17 7L7 17"},
        {"edit", "M12 20h9M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4z"},
        {"key", "M21 2l-2 2m-7.6 7.6a5.5 5.5 0 1 1-7.8 7.8 5.5 5.5 0 0 1 7.8-7.8zm0 0L15.5 7.5m0 0l3 3L22 7l-3-3m-3.5 3.5L19 4"},
        {"external", "M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6M15 3h6v6M10 14L21 3"},
        {"copy", "M9 9h11v11H9zM5 15H4V4h11v1"},
        {"lock", "M5 11h14v10H5zM8 11V7a4 4 0 0 1 8 0v4"},
        {"trash", "M4 7h16M10 11v6M14 11v6M5 7l1 13a1 1 0 0 0 1 1h10a1 1 0 0 0 1-1l1-13M9 7V4h6v3"},
    };
    return p;
}

} // namespace

QPixmap pixmap(const QString& name, const QColor& color, int size)
{
    const QString d = paths().value(name, paths().value(QStringLiteral("shield")));
    const QString svg = QStringLiteral("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='%1' "
                                       "stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='%2'/></svg>")
                            .arg(color.name(), d);
    QSvgRenderer renderer(svg.toUtf8());
    const qreal dpr = 2.0;
    QPixmap pm(QSize(size, size) * dpr);
    pm.fill(Qt::transparent);
    pm.setDevicePixelRatio(dpr);
    QPainter p(&pm);
    renderer.render(&p, QRectF(0, 0, size, size));
    return pm;
}

QIcon icon(const QString& name, const QColor& color, int size)
{
    return QIcon(pixmap(name, color, size));
}

QIcon trayIcon(const QColor& status)
{
    QPixmap pm(64, 64);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(status);
    p.setPen(Qt::NoPen);
    p.drawEllipse(QRectF(2, 2, 60, 60));
    p.drawPixmap(QRect(14, 14, 36, 36), pixmap(QStringLiteral("shield-check"), Qt::white, 36));
    return QIcon(pm);
}

} // namespace icons
