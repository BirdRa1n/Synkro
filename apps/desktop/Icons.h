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

// Ícone da bandeja: escudo sobre um círculo na cor do estado.
QIcon trayIcon(const QColor& status);

} // namespace icons
