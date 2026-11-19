#pragma once

#include "Ui.h"

#include <QDateTime>
#include <QLocale>
#include <QString>

#include <string>

// Formatação de datas, tamanhos e situações para as telas.
namespace fmt {

inline QString when(const QDateTime& dt)
{
    if (!dt.isValid())
        return QStringLiteral("—");
    const QDateTime local = dt.toLocalTime();
    const QDate today = QDate::currentDate();
    const QString hm = local.toString(QStringLiteral("HH:mm"));
    if (local.date() == today)
        return QStringLiteral("Hoje %1").arg(hm);
    if (local.date() == today.addDays(-1))
        return QStringLiteral("Ontem %1").arg(hm);
    return local.toString(QStringLiteral("dd/MM %1")).arg(hm);
}

inline QString bytes(std::uint64_t b)
{
    return QLocale::system().formattedDataSize(static_cast<qint64>(b), 1, QLocale::DataSizeTraditionalFormat);
}

inline QString statusText(const std::string& status)
{
    if (status == "success") return QStringLiteral("Sucesso");
    if (status == "degraded") return QStringLiteral("Degradado");
    if (status == "failed") return QStringLiteral("Falha");
    if (status == "blocked") return QStringLiteral("Bloqueado");
    if (status == "already_running") return QStringLiteral("Em execução");
    if (status == "interrupted") return QStringLiteral("Interrompido");
    return QString::fromStdString(status);
}

inline ui::Tone statusTone(const std::string& status)
{
    if (status == "success") return ui::Tone::Ok;
    if (status == "degraded" || status == "partial" || status == "already_running") return ui::Tone::Warn;
    if (status == "failed" || status == "blocked") return ui::Tone::Error;
    return ui::Tone::Neutral;
}

inline QString q(const std::string& s) { return QString::fromStdString(s); }
inline QString q(const std::wstring& s) { return QString::fromStdWString(s); }

} // namespace fmt
