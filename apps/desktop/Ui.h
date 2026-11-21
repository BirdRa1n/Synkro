#pragma once

// Peças visuais reutilizadas pelas telas (cartões, rótulos, barras, selos).

#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include <vector>

namespace ui {

enum class Tone { Neutral, Ok, Warn, Error, Accent };

QLabel* label(const QString& text, const char* role = nullptr, QWidget* parent = nullptr);
QFrame* card(const char* objectName = "card");
QPushButton* button(const QString& text, const QString& iconName = {}, bool primary = false);

// Recolore os ícones dos botões criados por button() (troca de tema).
void retintButtons(QWidget* root);

// Selo arredondado (ex.: "sucesso", "conectado").
QLabel* pill(const QString& text, Tone tone);

// Círculo com ícone (ok/aviso/erro) para listas de verificação.
QLabel* badge(Tone tone);

// Chip quadrado com ícone (canto superior dos cartões).
QLabel* iconChip(const QString& iconName, int size = 48);

// Remove todos os itens de um layout (reconstrução das telas).
void clearLayout(QLayout* layout);

// Barra de uso em dois segmentos.
class UsageBar : public QWidget {
public:
    explicit UsageBar(double used, double secondary = 0.0, QWidget* parent = nullptr);
    QSize sizeHint() const override { return {200, 10}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    double m_used, m_secondary;
};

// Ligações animadas do mapa de redundância (estilo editor de fluxo): cada
// origem liga-se, por uma curva, aos cofres que a tarefa usa. Cofre conectado
// = curva cheia com pulsos correndo em direção ao disco; ausente = tracejada
// e parada. As caixas ficam nas colunas vizinhas; a posição é lida delas.
class FlowLinks : public QWidget {
public:
    struct Link {
        QWidget* from;
        QWidget* to;
        bool active;
    };
    explicit FlowLinks(QWidget* parent = nullptr);
    void setLinks(std::vector<Link> links);
    QSize sizeHint() const override { return {140, 40}; }
    QSize minimumSizeHint() const override { return {90, 20}; }

protected:
    void paintEvent(QPaintEvent*) override;
    void timerEvent(QTimerEvent*) override;

private:
    std::vector<Link> m_links;
    int m_timer = 0;
    double m_phase = 0.0; // 0..1, avança com o tempo
};

// Base das telas: cabeçalho (título, subtítulo, ações) + conteúdo rolável.
class Page : public QWidget {
    Q_OBJECT
public:
    Page(const QString& title, const QString& subtitle, QWidget* parent = nullptr);
    virtual void refresh() = 0;

protected:
    QHBoxLayout* actions() const { return m_actions; }
    QVBoxLayout* body() const { return m_body; }

private:
    QHBoxLayout* m_actions;
    QVBoxLayout* m_body;
};

} // namespace ui
