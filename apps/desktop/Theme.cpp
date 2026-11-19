#include "Theme.h"

#include "Icons.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QGuiApplication>
#include <QPalette>
#include <QSettings>
#include <QStyleHints>

namespace {

ThemeTokens lightTokens()
{
    ThemeTokens t;
    t.bg = QColor("#F4F6FA");
    t.surface = QColor("#FFFFFF");
    t.inset = QColor("#F8FAFC");
    t.text = QColor("#0F172A");
    t.muted = QColor("#475569");
    t.border = QColor("#E2E8F0");
    t.borderStrong = QColor("#CBD5E1");
    t.nav = QColor("#0F172A");
    t.navActive = QColor("#1E293B");
    t.navInk = QColor("#CBD5E1");
    t.accent = QColor("#4338CA");
    t.button = QColor("#4338CA");
    t.button2 = QColor("#FFFFFF");
    t.chip = QColor("#EEF2FF");
    t.soft = QColor("#A5B4FC");
    t.track = QColor("#E2E8F0");
    t.dash = QColor("#94A3B8");
    t.okFg = QColor("#166534");
    t.okBg = QColor("#DCFCE7");
    t.warnFg = QColor("#92400E");
    t.warnBg = QColor("#FEF3C7");
    t.errFg = QColor("#991B1B");
    t.errBg = QColor("#FEE2E2");
    return t;
}

ThemeTokens darkTokens()
{
    ThemeTokens t;
    t.bg = QColor("#0B1020");
    t.surface = QColor("#121A2B");
    t.inset = QColor("#0E1626");
    t.text = QColor("#E2E8F0");
    t.muted = QColor("#94A3B8");
    t.border = QColor("#1F2A40");
    t.borderStrong = QColor("#334155");
    t.nav = QColor("#060A14");
    t.navActive = QColor("#1A2336");
    t.navInk = QColor("#94A3B8");
    t.accent = QColor("#818CF8");
    t.button = QColor("#4F46E5");
    t.button2 = QColor("#1A2336");
    t.chip = QColor("#1E1B4B");
    t.soft = QColor("#4C4FA8");
    t.track = QColor("#1F2A40");
    t.dash = QColor("#64748B");
    t.okFg = QColor("#4ADE80");
    t.okBg = QColor("#14532D");
    t.warnFg = QColor("#FBBF24");
    t.warnBg = QColor("#422006");
    t.errFg = QColor("#FCA5A5");
    t.errBg = QColor("#450A0A");
    return t;
}

QString c(const QColor& color)
{
    return color.name(QColor::HexRgb);
}

// A folha de estilo só aceita imagens por arquivo: os ícones das setas e da
// marca de seleção são gerados na cor do tema numa pasta de cache.
QString cachedIcon(const QString& name, const QColor& color, int size)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    const QString file = QDir(dir).filePath(QStringLiteral("%1-%2.png").arg(name, color.name().mid(1)));
    if (!QFileInfo::exists(file))
        icons::pixmap(name, color, size).save(file);
    return QDir::fromNativeSeparators(file);
}

} // namespace

Theme& Theme::instance()
{
    static Theme theme;
    return theme;
}

Theme::Theme() : m_light(lightTokens()), m_dark(darkTokens())
{
    QSettings s;
    m_mode = static_cast<ThemeMode>(s.value(QStringLiteral("ui/theme"), static_cast<int>(ThemeMode::System)).toInt());
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
        if (m_mode == ThemeMode::System) {
            apply();
            emit changed();
        }
    });
}

QString Theme::modeName(ThemeMode m)
{
    switch (m) {
    case ThemeMode::Light: return QStringLiteral("Claro");
    case ThemeMode::Dark:  return QStringLiteral("Escuro");
    case ThemeMode::System: break;
    }
    return QStringLiteral("Sistema");
}

void Theme::setMode(ThemeMode mode)
{
    if (mode == m_mode)
        return;
    m_mode = mode;
    QSettings().setValue(QStringLiteral("ui/theme"), static_cast<int>(mode));
    apply();
    emit changed();
}

bool Theme::isDark() const
{
    if (m_mode == ThemeMode::Dark)
        return true;
    if (m_mode == ThemeMode::Light)
        return false;
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

void Theme::apply()
{
    const ThemeTokens& t = tokens();
    QPalette p;
    p.setColor(QPalette::Window, t.bg);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Base, t.inset);
    p.setColor(QPalette::AlternateBase, t.surface);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::Button, t.button2);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::Highlight, t.button);
    p.setColor(QPalette::HighlightedText, QColor(Qt::white));
    p.setColor(QPalette::PlaceholderText, t.muted);
    p.setColor(QPalette::ToolTipBase, t.surface);
    p.setColor(QPalette::ToolTipText, t.text);
    p.setColor(QPalette::Link, t.accent);
    qApp->setPalette(p);
    qApp->setStyleSheet(styleSheet());
}

QString Theme::styleSheet() const
{
    const ThemeTokens& t = tokens();
    QString s = QStringLiteral(R"(
* { font-family: "Segoe UI Variable Text", "Segoe UI"; font-size: 10pt; }
QMainWindow, QDialog, QWizard, QWidget#page { background: %bg%; }
QWidget { color: %text%; }
QLabel { background: transparent; }
QLabel[role="h1"] { font-family: "Segoe UI Variable Display", "Segoe UI"; font-size: 20pt; font-weight: 700; }
QLabel[role="h2"] { font-family: "Segoe UI Variable Display", "Segoe UI"; font-size: 13pt; font-weight: 600; }
QLabel[role="muted"] { color: %muted%; }
QLabel[role="caption"] { color: %muted%; font-size: 8.5pt; font-weight: 600; letter-spacing: 1px; }
QLabel[role="fact"] { font-weight: 600; }

QWidget#navRail { background: %nav%; }
QToolButton#navButton { color: %navInk%; background: transparent; border: none; border-radius: 12px;
    padding: 6px 2px; font-size: 8pt; }
QToolButton#navButton:hover { background: %navActive%; }
QToolButton#navButton:checked { background: %navActive%; color: #FFFFFF; }
QToolButton#themeButton { color: %navInk%; background: transparent; border: none; border-radius: 8px; padding: 4px; }
QToolButton#themeButton:checked { background: %navActive%; color: #FFFFFF; }

QFrame#card { background: %surface%; border: 1px solid %border%; border-radius: 16px; }
QFrame#insetBox { background: %inset%; border: 1px solid %borderStrong%; border-radius: 10px; }
QFrame#vaultOn { background: transparent; border: 2px solid %accent%; border-radius: 12px; }
QFrame#vaultOff { background: transparent; border: 2px dashed %dash%; border-radius: 12px; }
QFrame#addCard { background: %surface%; border: 2px dashed %soft%; border-radius: 16px; }
QFrame#banner { border-radius: 14px; }

QPushButton { background: %button2%; color: %text%; border: 1px solid %borderStrong%; border-radius: 8px;
    padding: 8px 14px; min-height: 22px; }
QPushButton:hover { border-color: %accent%; }
QPushButton:disabled { color: %muted%; }
QPushButton[role="primary"] { background: %button%; color: #FFFFFF; border: none; font-weight: 600; }
QPushButton[role="primary"]:disabled { background: %soft%; }
QPushButton[role="danger"] { color: %errFg%; }

QLineEdit, QComboBox, QSpinBox, QTimeEdit, QDoubleSpinBox { background: %inset%; color: %text%;
    border: 1px solid %borderStrong%; border-radius: 8px; padding: 6px 8px; min-height: 22px; }
QLineEdit:focus, QComboBox:focus, QSpinBox:focus, QTimeEdit:focus { border-color: %accent%; }
QComboBox QAbstractItemView { background: %surface%; color: %text%; selection-background-color: %button%; }
QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: center right; width: 28px; border: none; }
QComboBox::down-arrow { image: url("%chevron%"); width: 14px; height: 14px; }
QTimeEdit::up-button, QTimeEdit::down-button, QSpinBox::up-button, QSpinBox::down-button { width: 0; border: none; }
QCheckBox::indicator, QRadioButton::indicator { width: 16px; height: 16px; border: 1.5px solid %borderStrong%;
    background: %inset%; }
QCheckBox::indicator { border-radius: 4px; }
QRadioButton::indicator { border-radius: 9px; }
QCheckBox::indicator:checked { background: %button%; border-color: %button%; image: url("%check%"); }
QRadioButton::indicator:checked { background: %button%; border: 4px solid %inset%; outline: none; }
QTreeView::item, QTableView::item, QListView::item { padding: 7px 8px; border: none; }
QTreeView::item:selected, QTableView::item:selected, QListView::item:selected { background: %chip%; color: %text%; }
QPlainTextEdit, QTextBrowser, QTextEdit { background: %inset%; color: %text%; border: 1px solid %borderStrong%;
    border-radius: 10px; padding: 6px; }
QPlainTextEdit[role="code"] { font-family: "Cascadia Mono", "Consolas"; font-size: 10pt; }

QTableWidget, QTreeWidget, QListWidget { background: %surface%; alternate-background-color: %inset%; color: %text%;
    border: 1px solid %border%; border-radius: 12px; gridline-color: %border%; selection-background-color: %chip%;
    selection-color: %text%; }
QHeaderView::section { background: %surface%; color: %muted%; border: none; border-bottom: 1px solid %border%;
    padding: 8px; font-weight: 600; }
QCheckBox, QRadioButton { background: transparent; spacing: 8px; }
QScrollArea { background: transparent; border: none; }
QScrollArea > QWidget > QWidget { background: transparent; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: %borderStrong%; border-radius: 4px; min-height: 30px; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
QToolTip { background: %surface%; color: %text%; border: 1px solid %border%; padding: 6px; }
QProgressBar { background: %track%; border: none; border-radius: 4px; height: 8px; text-align: center; }
QProgressBar::chunk { background: %accent%; border-radius: 4px; }
)");
    const std::pair<const char*, QColor> map[] = {
        {"%bg%", t.bg}, {"%surface%", t.surface}, {"%inset%", t.inset}, {"%text%", t.text}, {"%muted%", t.muted},
        {"%border%", t.border}, {"%borderStrong%", t.borderStrong}, {"%nav%", t.nav}, {"%navActive%", t.navActive},
        {"%navInk%", t.navInk}, {"%accent%", t.accent}, {"%button%", t.button}, {"%button2%", t.button2},
        {"%chip%", t.chip}, {"%soft%", t.soft}, {"%track%", t.track}, {"%dash%", t.dash}, {"%errFg%", t.errFg},
    };
    for (const auto& [key, color] : map)
        s.replace(QLatin1String(key), c(color));
    s.replace(QLatin1String("%chevron%"), cachedIcon(QStringLiteral("chevron-down"), t.muted, 14));
    s.replace(QLatin1String("%check%"), cachedIcon(QStringLiteral("check"), QColor(Qt::white), 12));
    return s;
}
