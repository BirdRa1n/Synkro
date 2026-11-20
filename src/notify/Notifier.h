#pragma once

// Notificações externas (configuration.md §3.1): e-mail, webhook e Telegram.
//
// Os alertas continuam sendo calculados por state/Alerts. Este módulo só
// decide o que avisar a cada canal e entrega: tudo passa por uma fila no
// banco de estado (outbox), para que uma rede fora do ar adie o aviso em vez
// de perdê-lo. Só o worker (SYSTEM) enfileira e envia; ele é o único que lê
// os segredos e roda mesmo sem ninguém logado.

#include "config/Config.h"
#include "core/Result.h"
#include "state/StateStore.h"

#include <QDateTime>

#include <string>
#include <utility>
#include <vector>

namespace synkro::notify {

// Mensagem pronta para um canal (um lote de itens da fila).
struct Composed {
    std::string instance;
    std::string title;           // assunto do e-mail / título no chat
    std::string text;            // corpo em texto simples
    std::vector<OutboxRow> items;
};

struct HttpRequest {
    std::string url;
    std::string body;            // JSON
    std::vector<std::pair<std::string, std::string>> headers;
    bool insecureTls = false;    // allow_insecure: aceita certificado não confiável
};

std::string instanceName(const config::GlobalConfig& g);

Composed compose(const std::string& instance, const std::vector<OutboxRow>& items);

// Montagem por canal (sem rede; testável).
HttpRequest buildWebhook(const config::NotificationChannel& ch, const Composed& m, const std::string& token);
HttpRequest buildTelegram(const config::NotificationChannel& ch, const Composed& m, const std::string& botToken);
std::string buildEmail(const config::NotificationChannel& ch, const Composed& m, const QDateTime& date);

// Decide o que avisar: alertas novos para o canal, lembretes de alertas que
// continuam abertos, resoluções e o relatório diário.
struct EnqueueResult {
    int alerts = 0, reminders = 0, resolved = 0, reports = 0;
};
EnqueueResult enqueue(const config::ConfigSet& set, StateStore& state, const QDateTime& nowUtc);

struct DeliveryOptions {
    std::wstring secretsFile;    // vazio = padrão
    QDateTime now;               // inválido = agora
    bool ignoreBackoff = false;  // notify --now: tenta mesmo os adiados
};

struct ChannelDelivery {
    std::string channel;
    int items = 0;
    bool ok = false;
    std::string error;
};

struct DeliveryReport {
    std::vector<ChannelDelivery> channels;
    int expired = 0;
};

// Entrega o que está pendente, um lote por canal. Falhas ficam na fila com
// nova tentativa em 5min, 10min, 20min... até 1h; itens com mais de 3 dias
// expiram.
DeliveryReport deliver(const config::ConfigSet& set, StateStore& state, const DeliveryOptions& options);

// Envia uma mensagem de teste direto ao canal (fora da fila).
Status sendTest(const config::ConfigSet& set, const std::string& channelId, const std::wstring& secretsFile);

// Envia um lote já montado pelo canal (resolve os segredos).
Status send(const config::NotificationChannel& ch, const Composed& m, const std::wstring& secretsFile);

// Transporte.
Status sendHttp(const HttpRequest& req, std::int64_t timeoutSeconds, std::string* responseBody = nullptr);
Status sendSmtp(const config::NotificationChannel& ch, const std::string& password, const std::string& message);

} // namespace synkro::notify
