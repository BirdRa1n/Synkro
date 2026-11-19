#pragma once

#include <QColor>
#include <QObject>
#include <QString>

// Tema do desktop (proposta de design 5, "Cofres"): claro, escuro ou o do
// Windows. A escolha é preferência de quem usa o computador e fica nas
// configurações do usuário (QSettings), não no YAML.

enum class ThemeMode { System, Light, Dark };

struct ThemeTokens {
    QColor bg, surface, inset, text, muted, border, borderStrong;
    QColor nav, navActive, navInk;
    QColor accent, button, button2, chip, soft, track, dash;
    QColor okFg, okBg, warnFg, warnBg, errFg, errBg;
};

class Theme : public QObject {
    Q_OBJECT
public:
    static Theme& instance();

    ThemeMode mode() const { return m_mode; }
    void setMode(ThemeMode mode);
    bool isDark() const;
    const ThemeTokens& tokens() const { return isDark() ? m_dark : m_light; }

    // Aplica paleta e folha de estilo ao aplicativo inteiro.
    void apply();

    static QString modeName(ThemeMode m);

signals:
    void changed();

private:
    Theme();
    QString styleSheet() const;

    ThemeMode m_mode = ThemeMode::System;
    ThemeTokens m_light;
    ThemeTokens m_dark;
};

// Atalho: cor do tema atual.
inline const ThemeTokens& tk()
{
    return Theme::instance().tokens();
}
