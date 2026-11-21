// Gera o synkro.ico (16 a 256 px) e os PNGs a partir dos SVGs do ícone.
//
//   icongen <synkro.svg> <synkro-small.svg> <pasta de saída>
//
// Até 32 px usa o desenho simplificado. As entradas até 128 px vão como DIB
// 32 bits (compatível com qualquer leitor de .ico); só a de 256 px vai como PNG,
// o padrão dos ícones do próprio Windows.

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QSvgRenderer>

#include <cstdio>
#include <vector>

namespace {

QImage render(QSvgRenderer& svg, int size)
{
    QImage img(size, size, QImage::Format_ARGB32);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    svg.render(&p, QRectF(0, 0, size, size));
    return img;
}

void putU16(QByteArray& b, quint16 v)
{
    b.append(char(v & 0xFF)).append(char(v >> 8));
}

void putU32(QByteArray& b, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        b.append(char((v >> (8 * i)) & 0xFF));
}

// BITMAPINFOHEADER + pixels BGRA de baixo para cima + máscara AND (zerada).
QByteArray dib(const QImage& img)
{
    const int s = img.width();
    QByteArray b;
    putU32(b, 40);
    putU32(b, quint32(s));
    putU32(b, quint32(s * 2)); // altura dobrada: cor + máscara
    putU16(b, 1);
    putU16(b, 32);
    putU32(b, 0);
    putU32(b, 0);
    putU32(b, 0);
    putU32(b, 0);
    putU32(b, 0);
    putU32(b, 0);
    for (int y = s - 1; y >= 0; --y) {
        const auto* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < s; ++x) {
            const QRgb px = line[x]; // ARGB32 não pré-multiplicado
            b.append(char(qBlue(px))).append(char(qGreen(px))).append(char(qRed(px))).append(char(qAlpha(px)));
        }
    }
    const int maskRow = ((s + 31) / 32) * 4;
    b.append(QByteArray(maskRow * s, '\0'));
    return b;
}

QByteArray png(const QImage& img)
{
    QByteArray b;
    QBuffer buf(&b);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return b;
}

} // namespace

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    if (argc != 4) {
        std::fprintf(stderr, "uso: icongen <synkro.svg> <synkro-small.svg> <pasta>\n");
        return 64;
    }
    QSvgRenderer big(QString::fromLocal8Bit(argv[1]));
    QSvgRenderer small(QString::fromLocal8Bit(argv[2]));
    if (!big.isValid() || !small.isValid()) {
        std::fprintf(stderr, "SVG inválido\n");
        return 2;
    }
    const QDir out(QString::fromLocal8Bit(argv[3]));
    QDir().mkpath(out.path());

    const std::vector<int> sizes = {16, 20, 24, 32, 40, 48, 64, 128, 256};
    std::vector<QByteArray> data;
    for (int s : sizes) {
        const QImage img = render(s <= 32 ? small : big, s);
        data.push_back(s < 256 ? dib(img) : png(img));
        if (s == 16 || s == 32 || s == 48 || s == 256)
            img.save(out.filePath(QStringLiteral("synkro-%1.png").arg(s)), "PNG");
    }

    QByteArray ico;
    putU16(ico, 0);
    putU16(ico, 1); // ícone
    putU16(ico, quint16(sizes.size()));
    quint32 offset = 6 + 16 * quint32(sizes.size());
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const int s = sizes[i];
        ico.append(char(s >= 256 ? 0 : s)).append(char(s >= 256 ? 0 : s)).append(char(0)).append(char(0));
        putU16(ico, 1);
        putU16(ico, 32);
        putU32(ico, quint32(data[i].size()));
        putU32(ico, offset);
        offset += quint32(data[i].size());
    }
    for (const auto& d : data)
        ico.append(d);

    QFile f(out.filePath(QStringLiteral("synkro.ico")));
    if (!f.open(QIODevice::WriteOnly) || f.write(ico) != ico.size()) {
        std::fprintf(stderr, "não foi possível gravar o .ico\n");
        return 2;
    }
    std::printf("synkro.ico: %d tamanhos, %lld bytes\n", int(sizes.size()), static_cast<long long>(ico.size()));
    return 0;
}
