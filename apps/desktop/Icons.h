#pragma once

#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QString>

// Ícones de traço (estilo da proposta de design), desenhados a partir de
// caminhos SVG na cor pedida — acompanham o tema sem arquivos de imagem.
namespace icons {

QIcon icon(const QString& name, const QColor& color, int size = 22);
QPixmap pixmap(const QString& name, const QColor& color, int size = 22);

// Ícone do Synkro (porta de cofre, assets/icons): desenho simplificado até
// 32 px, completo acima disso.
QPixmap appPixmap(int size);
QIcon appIcon();

// Ícone da bandeja: o do Synkro com um ponto na cor do estado (verde, amarelo
// ou vermelho) no canto inferior direito.
QIcon trayIcon(const QColor& status);

} // namespace icons
