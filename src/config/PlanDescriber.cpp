#include "config/PlanDescriber.h"

#include "config/Values.h"
#include "platform/windows/Win32.h"

#include <sstream>

namespace synkro::config {

std::string describeTrigger(const ScheduleTrigger& t)
{
    switch (t.kind) {
    case TriggerKind::Daily:
        return "Todos os dias às " + describeTime(t.atMinutes) + ".";
    case TriggerKind::Weekly: {
        std::string days = describeWeekdays(t.days);
        if (!days.empty())
            days[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(days[0])));
        return days + ", às " + describeTime(t.atMinutes) + ".";
    }
    case TriggerKind::Monthly:
        return t.dayOfMonth < 0 ? "No último dia de cada mês, às " + describeTime(t.atMinutes) + "."
                                : "Todo dia " + std::to_string(t.dayOfMonth) + " do mês, às " + describeTime(t.atMinutes) + ".";
    case TriggerKind::Every: {
        std::string s = "A cada " + describeDuration(t.intervalSeconds);
        if (t.betweenStart >= 0)
            s += ", entre " + describeTime(t.betweenStart) + " e " + describeTime(t.betweenEnd);
        if (!t.days.empty())
            s += ", " + describeWeekdays(t.days);
        return s + ".";
    }
    case TriggerKind::OnStartup:
        return t.delaySeconds > 0 ? "Ao ligar o computador, " + describeDuration(t.delaySeconds) + " depois."
                                  : "Ao ligar o computador.";
    case TriggerKind::OnLogon:
        return t.delaySeconds > 0 ? "Quando um usuário entrar no Windows, " + describeDuration(t.delaySeconds) + " depois."
                                  : "Quando um usuário entrar no Windows.";
    case TriggerKind::Manual:
        return "Somente sob demanda (sem agendamento).";
    }
    return {};
}

namespace {

std::string vaultLabel(const ConfigSet& set, const std::string& id)
{
    const VaultConfig* v = set.global.findVault(id);
    return v ? v->label : id;
}

void describeVaults(std::ostringstream& o, const ConfigSet& set, const JobConfig& job, int step)
{
    o << "  " << step << ". Gravar nos cofres:\n";
    for (const auto& v : job.vaults) {
        const VaultConfig* vc = set.global.findVault(v);
        o << "       • " << vaultLabel(set, v);
        if (vc && vc->rotation)
            o << " (rodízio: pode estar ausente até " << describeDuration(vc->maxAbsenceSeconds) << ")";
        o << "\n";
    }
    const size_t total = job.vaults.size();
    const size_t required = job.minVaultsOk == 0 ? total : static_cast<size_t>(job.minVaultsOk);
    if (total > 1) {
        if (required >= total)
            o << "     Sucesso exige os " << total << " cofres; com menos, o resultado será \"Degradado\".\n";
        else
            o << "     Sucesso exige " << required << " de " << total << " cofres.\n";
    }
}

void describeFiles(std::ostringstream& o, const ConfigSet& set, const JobConfig& job)
{
    const FilesJob& f = job.files;
    int step = 1;
    if (f.moreSources.empty()) {
        o << "  " << step++ << ". Ler a pasta " << win::toUtf8(f.source);
    } else {
        o << "  " << step++ << ". Ler " << (f.moreSources.size() + 1) << " pastas:\n"
          << "       • " << win::toUtf8(f.source) << " (no cofre em " << win::toUtf8(f.sourceAlias) << "\\)\n";
        for (const auto& s : f.moreSources)
            o << "       • " << win::toUtf8(s.path) << " (no cofre em " << win::toUtf8(s.alias) << "\\)\n";
        o << "     ";
    }
    if (f.vss)
        o << " a partir de um snapshot VSS (arquivos em uso ficam consistentes; exige execução como "
             "administrador/SYSTEM)"
          << (f.continueWithoutSnapshot ? "; se o snapshot falhar, copia sem ele e avisa" : "; se o snapshot falhar, a tarefa falha");
    o << ".\n";
    if (!f.include.empty()) {
        o << "     Somente:";
        for (const auto& p : f.include)
            o << " " << win::toUtf8(p);
        o << "\n";
    }
    if (!f.exclude.empty()) {
        o << "     Ignorando:";
        for (const auto& p : f.exclude)
            o << " " << win::toUtf8(p);
        o << "\n";
    }
    o << "  " << step++ << ". Comparar com o último backup de cada cofre e copiar apenas arquivos novos ou alterados.\n";
    o << "     Versões anteriores e arquivos excluídos na origem vão para versions/ — nada é apagado.\n";
    o << "  " << step++ << ". Conferir cada cópia com SHA-256"
      << (f.verifyUncached ? ", relendo do disco sem cache" : " calculado durante a cópia") << ".\n";
    describeVaults(o, set, job, step++);
    o << "  " << step++ << ". Apagar versões antigas mantendo " << describeDuration(f.keepVersionsSeconds)
      << " de histórico (no mínimo as " << f.keepMinVersions << " execuções mais recentes) e arquivos excluídos por "
      << describeDuration(f.keepDeletedSeconds) << ".\n";

    o << "\nPROTEÇÕES\n";
    o << (f.moreSources.empty() ? "  • Se a pasta de origem sumir ou ficar vazia, nada é alterado nos cofres e um alerta é aberto.\n"
                                : "  • Se qualquer uma das pastas sumir ou ficar vazia, nada é alterado nos cofres e um alerta é aberto.\n");
    o << "  • Se mais de " << describePercent(f.maxDeleted) << " dos arquivos sumirem ou mais de "
      << describePercent(f.maxModified) << " forem alterados de uma vez, a execução é bloqueada até confirmação.\n";
    if (!f.sentinelFile.empty())
        o << "  • A origem só é aceita se o arquivo " << win::toUtf8(f.sentinelFile) << " existir.\n";
}

void describeDatabase(std::ostringstream& o, const ConfigSet& set, const JobConfig& job)
{
    const DatabaseJob& d = job.database;
    int step = 1;
    if (d.engine == "sqlite") {
        o << "  " << step++ << ". Copiar com a API de backup do SQLite (consistente mesmo com o banco em uso):\n";
        for (const auto& f : d.sqliteFiles)
            o << "       • " << win::toUtf8(f) << "\n";
        o << "  " << step++ << ". Conferir cada cópia com PRAGMA integrity_check.\n";
    } else if (d.engine == "custom") {
        o << "  " << step++ << ". Executar o comando personalizado:";
        for (const auto& a : d.customCommand)
            o << " " << a;
        o << "\n";
    } else {
        const std::string engineName = d.engine == "postgresql" ? "PostgreSQL"
                                       : d.engine == "mysql"      ? "MySQL"
                                       : d.engine == "firebird"   ? "Firebird"
                                       : d.engine == "sqlserver"  ? "SQL Server"
                                                                  : "MariaDB";
        o << "  " << step++ << ". Conectar ao " << engineName << " em " << d.host;
        if (d.port > 0)
            o << ":" << d.port;
        if (d.windowsAuth)
            o << " com a autenticação do Windows (conta do serviço do Synkro)";
        else
            o << " como \"" << d.user << "\"";
        if (d.password.kind == SecretRef::Kind::Secret)
            o << " (senha do segredo \"" << d.password.name << "\")";
        else if (d.password.kind == SecretRef::Kind::Env)
            o << " (senha da variável " << d.password.name << ")";
        if (!d.sslMode.empty())
            o << ", criptografia " << d.sslMode;
        o << ", até " << d.retries << " tentativas.\n";
        if (d.remote)
            o << "     Servidor remoto: o dump trafega pela rede"
              << (d.serverLock ? " e uma trava no servidor impede dumps simultâneos de outras máquinas" : "") << ".\n";
        o << "  " << step++ << ". Exportar ";
        if (d.allDatabases) {
            o << "todos os bancos";
        } else {
            for (size_t i = 0; i < d.databases.size(); ++i) {
                o << (i ? ", " : "") << d.databases[i].name;
                if (d.engine == "firebird" && d.databases[i].path != d.databases[i].name)
                    o << " (" << d.databases[i].path << ")";
            }
        }
        if (d.engine == "postgresql")
            o << " com pg_dump (formato " << (d.options.count("format") ? d.options.at("format") : "custom")
              << ", um instante único do banco sem travar o PDV).\n";
        else if (d.engine == "firebird")
            o << " com gbak -b (instante único da transação, sem parar o PDV; senha por arquivo temporário).\n";
        else if (d.engine == "sqlserver")
            o << " com BACKUP DATABASE ... WITH COPY_ONLY, CHECKSUM (não interfere na cadeia de backups de log) em "
              << (d.options.count("backup_dir") ? d.options.at("backup_dir") : std::string("pasta de backup da instância"))
              << "; o arquivo é trazido para esta máquina e apagado de lá.\n";
        else
            o << " com mysqldump --single-transaction (instante único para tabelas InnoDB).\n";
        const char* validation = d.engine == "postgresql" ? "pg_restore --list"
                                 : d.engine == "firebird"  ? "término registrado no log do gbak"
                                 : d.engine == "sqlserver" ? "RESTORE VERIFYONLY WITH CHECKSUM"
                                                           : "marca \"Dump completed\"";
        o << "  " << step++ << ". Validar cada dump antes de gravar (" << validation << " e número de tabelas).\n";
    }
    describeVaults(o, set, job, step++);
    o << "  " << step++ << ". Manter os dumps: últimos " << d.keepLast << ", 1 por dia por " << d.keepDaily
      << " dias, 1 por semana por " << d.keepWeekly << " semanas, 1 por mês por " << d.keepMonthly << " meses.\n";

    o << "\nPROTEÇÕES\n";
    o << "  • Banco vazio: " << (d.blockOnEmptyDatabase ? "nada é gravado e um alerta crítico é aberto" : "apenas alerta")
      << ".\n";
    o << "  • Dump menor que " << describePercent(d.minSizeRatio)
      << " do anterior: alerta crítico e nenhum dump antigo é apagado.\n";
    if (d.serverLock && d.engine != "sqlite" && d.engine != "custom")
        o << "  • Trava no servidor (" << (d.engine == "postgresql" ? "pg_try_advisory_lock" : "GET_LOCK")
          << "): se outra máquina já estiver exportando o mesmo banco, este dump é ignorado.\n";

    if (d.restoreTest) {
        const auto& rt = d.restoreTestConfig;
        o << "\nTESTE DE RESTAURAÇÃO\n";
        o << "  " << (rt.schedule ? describeTrigger(*rt.schedule) : std::string("Toda semana, domingo às 04:00"))
          << ": restaura o dump mais recente de cada banco ";
        if (d.engine == "sqlite")
            o << "num arquivo temporário";
        else
            o << "num banco temporário synkro_verify_<id> em "
              << (rt.sameConnection ? d.host + ":" + std::to_string(d.port) + " (o mesmo servidor)"
                                    : rt.host + ":" + std::to_string(rt.port) + " (servidor de teste)");
        o << ", confere as tabelas registradas no dump";
        if (!rt.checks.empty())
            o << ", roda " << rt.checks.size() << " verificação(ões)";
        o << " e apaga o temporário. Falha abre alerta crítico.\n";
    }
}

// Recursos configurados que esta versão ainda não executa. O plano precisa
// deixar isso explícito: o usuário não pode supor uma proteção que não existe.
std::vector<std::string> pendingFeatures(const JobConfig& job)
{
    std::vector<std::string> out;
    if (job.type == "database" && job.database.restoreTest && job.database.engine == "custom")
        out.push_back("o teste de restauração automático para comandos personalizados (use restore-db manualmente)");
    return out;
}

} // namespace

namespace {

} // namespace

std::string describeJob(const ConfigSet& set, const JobConfig& job)
{
    std::ostringstream o;
    o << "Tarefa \"" << job.name << "\" [" << job.id << "] — "
      << (job.type == "files" ? "backup de arquivos" : "backup de banco de dados");
    if (!job.enabled)
        o << " (DESATIVADA)";
    o << "\n\nQUANDO\n";
    if (job.schedule.empty())
        o << "  • Somente sob demanda (sem agendamento).\n";
    for (const auto& t : job.schedule)
        o << "  • " << describeTrigger(t) << "\n";
    if (set.global.scheduleCatchUp && !job.schedule.empty())
        o << "  • Se o computador estiver desligado no horário, executa assim que ligar.\n";
    if (job.timeoutSeconds > 0)
        o << "  • Tempo máximo: " << describeDuration(job.timeoutSeconds) << ".\n";
    if (!job.dependsOn.empty()) {
        o << "  • Começa depois de:";
        for (const auto& d : job.dependsOn)
            o << " " << d;
        o << (job.dependsOnSuccess ? " (somente se terminarem com sucesso)" : "") << ".\n";
    }

    if (!job.hooks.before.empty()) {
        o << "\nANTES\n";
        for (const auto& h : job.hooks.before) {
            o << "  •";
            for (const auto& a : h.run)
                o << " " << a;
            o << (h.failOnError ? "  (se falhar, a tarefa falha)" : "  (se falhar, apenas avisa)") << "\n";
        }
    }

    o << "\nO QUE SERÁ FEITO\n";
    if (job.type == "files")
        describeFiles(o, set, job);
    else
        describeDatabase(o, set, job);

    std::vector<std::string> dependents;
    for (const auto& other : set.jobs) {
        for (const auto& d : other.dependsOn) {
            if (d == job.id)
                dependents.push_back(other.name);
        }
    }
    if (!job.hooks.after.empty() || !dependents.empty() || job.shutdownAfter) {
        o << "\nDEPOIS\n";
        for (const auto& h : job.hooks.after) {
            o << "  •";
            for (const auto& a : h.run)
                o << " " << a;
            o << "\n";
        }
        for (const auto& n : dependents)
            o << "  • A tarefa \"" << n << "\" começa em seguida (depende desta).\n";
        if (job.shutdownAfter)
            o << "  • Desligar o computador após " << describeDuration(set.global.shutdownCountdownSeconds)
              << " de aviso, somente se tudo terminar com sucesso.\n";
    }

    const auto pending = pendingFeatures(job);
    if (!pending.empty()) {
        o << "\nAINDA NÃO DISPONÍVEL NESTA VERSÃO\n";
        for (const auto& p : pending)
            o << "  • " << p << "\n";
    }
    return o.str();
}

} // namespace synkro::config
