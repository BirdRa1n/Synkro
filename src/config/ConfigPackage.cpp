#include "config/ConfigPackage.h"

#include "config/ConfigLoader.h"
#include "config/ConfigWriter.h"
#include "core/RunId.h"
#include "engine/VaultOps.h"
#include "platform/windows/Win32.h"
#include "secrets/SecretStore.h"
#include "core/Paths.h"
#include "vault/Vault.h"

#include <QDateTime>
#include <QSysInfo>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <set>

namespace synkro::config {

namespace {

constexpr char kKind[] = "synkro-config-package";

std::string withoutCr(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != '\r')
            out += c;
    return out;
}

// Só synkro.yml e jobs/<nome>.yml, sem subir de pasta.
bool safeRelativePath(const std::wstring& p)
{
    if (p == L"synkro.yml")
        return true;
    if (p.rfind(L"jobs\\", 0) != 0 || p.size() <= 9 || p.compare(p.size() - 4, 4, L".yml") != 0)
        return false;
    const std::wstring name = p.substr(5);
    return name.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos && name.find(L"..") == std::wstring::npos;
}

std::string text(const std::wstring& path)
{
    auto r = win::readWholeFile(path, 4u << 20);
    return r ? std::move(r).value() : std::string();
}

bool exists(const std::wstring& path)
{
    return ::GetFileAttributesW(win::toExtendedPath(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

Result<std::string> exportPackage(const ConfigSet& set, const std::vector<std::string>& jobIds)
{
    if (set.hasErrors())
        return Error::make(ErrorCode::InvalidArgument, "a configuração tem " + std::to_string(set.errorCount())
                                                           + " erro(s); corrija antes de exportar");
    std::vector<const JobConfig*> jobs;
    for (const auto& id : jobIds) {
        const JobConfig* j = set.findJob(id);
        if (!j)
            return Error::make(ErrorCode::NotFound, "tarefa \"" + id + "\" não encontrada");
        jobs.push_back(j);
    }
    if (jobIds.empty())
        for (const auto& j : set.jobs)
            jobs.push_back(&j);

    std::set<std::string> secrets;
    for (const auto* j : jobs)
        for (const auto& s : j->secrets)
            secrets.insert(s.name);

    // A identidade dos discos é desta máquina: sai do pacote.
    GlobalConfig global = set.global;
    for (auto& v : global.vaults) {
        v.vaultId.clear();
        v.path.clear();
    }

    YAML::Emitter e;
    e << YAML::Comment("Pacote de configuração do Synkro. Não contém senhas nem a identidade dos discos.");
    e << YAML::BeginDoc << YAML::BeginMap;
    e << YAML::Key << "kind" << YAML::Value << kKind;
    e << YAML::Key << "version" << YAML::Value << 1;
    e << YAML::Key << "exported_from" << YAML::Value << QSysInfo::machineHostName().toStdString();
    e << YAML::Key << "exported_at" << YAML::Value
      << QDateTime::currentDateTimeUtc().toString(Qt::ISODate).toStdString();
    e << YAML::Key << "secrets_required" << YAML::Value << YAML::Flow << YAML::BeginSeq;
    for (const auto& s : secrets)
        e << s;
    e << YAML::EndSeq;
    e << YAML::Key << "vaults" << YAML::Value << YAML::BeginSeq;
    for (const auto& v : global.vaults) {
        e << YAML::Flow << YAML::BeginMap << YAML::Key << "id" << YAML::Value << v.id << YAML::Key << "label" << YAML::Value
          << v.label << YAML::Key << "rotation" << YAML::Value << v.rotation << YAML::EndMap;
    }
    e << YAML::EndSeq << YAML::EndMap;

    auto addFile = [&](const std::string& rel, const std::string& content) {
        e << YAML::BeginDoc << YAML::BeginMap;
        e << YAML::Key << "file" << YAML::Value << rel;
        e << YAML::Key << "content" << YAML::Value << YAML::Literal << withoutCr(content);
        e << YAML::EndMap;
    };
    addFile("synkro.yml", emitGlobal(global));
    for (const auto* j : jobs) {
        const std::string content = text(win::joinPath(set.directory, j->file));
        if (content.empty())
            return Error::make(ErrorCode::IoError, "não foi possível ler " + win::toUtf8(j->file));
        std::string rel = win::toUtf8(j->file);
        std::replace(rel.begin(), rel.end(), '\\', '/');
        addFile(rel, content);
    }
    if (!e.good())
        return Error::make(ErrorCode::Internal, std::string("falha ao gerar o pacote: ") + e.GetLastError());
    return std::string(e.c_str()) + "\n";
}

Result<ConfigPackage> parsePackage(const std::string& data)
{
    std::vector<YAML::Node> docs;
    try {
        docs = YAML::LoadAll(data);
    } catch (const YAML::Exception& ex) {
        return Error::make(ErrorCode::Corrupted, std::string("pacote inválido: ") + ex.what());
    }
    if (docs.empty() || !docs[0].IsMap() || !docs[0]["kind"] || docs[0]["kind"].as<std::string>("") != kKind)
        return Error::make(ErrorCode::Corrupted, "o arquivo não é um pacote de configuração do Synkro");
    if (docs[0]["version"].as<int>(0) != 1)
        return Error::make(ErrorCode::Unsupported, "versão de pacote não suportada");

    ConfigPackage p;
    const YAML::Node& h = docs[0];
    p.exportedFrom = h["exported_from"].as<std::string>("");
    p.exportedAt = h["exported_at"].as<std::string>("");
    if (h["secrets_required"] && h["secrets_required"].IsSequence())
        for (const auto& s : h["secrets_required"])
            p.secrets.push_back(s.as<std::string>(""));
    if (h["vaults"] && h["vaults"].IsSequence()) {
        for (const auto& v : h["vaults"]) {
            VaultConfig vc;
            vc.id = v["id"].as<std::string>("");
            vc.label = v["label"].as<std::string>("");
            vc.rotation = v["rotation"].as<bool>(false);
            p.vaults.push_back(vc);
        }
    }
    for (size_t i = 1; i < docs.size(); ++i) {
        const YAML::Node& d = docs[i];
        if (!d.IsMap() || !d["file"] || !d["content"])
            return Error::make(ErrorCode::Corrupted, "documento " + std::to_string(i + 1) + " do pacote sem file/content");
        std::wstring rel = win::fromUtf8(d["file"].as<std::string>(""));
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        if (!safeRelativePath(rel))
            return Error::make(ErrorCode::Corrupted, "caminho não permitido no pacote: " + win::toUtf8(rel));
        p.files.push_back({rel, d["content"].as<std::string>("")});
    }
    return p;
}

Result<ImportReport> importPackage(const ConfigPackage& pkg, const ImportOptions& opt)
{
    ImportReport report;
    if (opt.configDir.empty())
        return Error::make(ErrorCode::InvalidArgument, "pasta de configuração não informada");
    const std::wstring targetGlobal = win::joinPath(opt.configDir, L"synkro.yml");
    const bool hasGlobal = exists(targetGlobal);

    // Pasta temporária ao lado da configuração: monta, valida e só então grava.
    const std::wstring temp = opt.configDir + L".import-" + newRunId();
    struct Cleanup {
        std::wstring dir;
        ~Cleanup() { (void)ops::removeTree(dir); }
    } cleanup{temp};
    if (auto st = win::createDirectories(win::joinPath(temp, L"jobs")); !st)
        return st.error();
    if (hasGlobal)
        (void)win::writeFileAtomically(win::joinPath(temp, L"synkro.yml"), text(targetGlobal));
    for (const auto& f : ops::listFiles(win::joinPath(opt.configDir, L"jobs"), L".yml"))
        (void)win::writeFileAtomically(win::joinPath(temp, L"jobs\\" + f),
                                       text(win::joinPath(win::joinPath(opt.configDir, L"jobs"), f)));

    // Liga cada cofre do pacote a um disco desta máquina.
    std::map<std::string, vault::VaultIdentity> bound;
    for (const auto& [id, location] : opt.bindings) {
        auto identity = vault::readIdentity(location);
        if (!identity)
            return Error::make(ErrorCode::NotFound, "--bind " + id + ": nenhum cofre do Synkro em " + win::toUtf8(location)
                                                        + " (prepare o disco com \"vault init\" ou pelo desktop)");
        bound[id] = identity.value();
    }
    auto bindingFor = [&](VaultConfig& v, const std::wstring& location) {
        const auto& ident = bound.at(v.id);
        v.vaultId = ident.vaultId.toStdString();
        std::wstring root = location;
        if (root.size() == 2 && root[1] == L':')
            root += L'\\';
        v.path = root;
    };

    // Tarefas do pacote: quais arquivos entram e quais cofres elas usam.
    GlobalConfig pkgGlobal;
    std::vector<Diagnostic> ignored;
    std::set<std::string> vaultsUsed;
    for (const auto& f : pkg.files) {
        if (f.relativePath == L"synkro.yml") {
            pkgGlobal = parseGlobalDocument(f.content, L"synkro.yml", ignored);
            continue;
        }
        const std::wstring dest = win::joinPath(opt.configDir, f.relativePath);
        const std::string rel = win::toUtf8(f.relativePath);
        if (exists(dest)) {
            if (withoutCr(text(dest)) == withoutCr(f.content)) {
                report.kept.push_back(rel + " (igual)");
                continue;
            }
            if (!opt.replaceJobs) {
                report.kept.push_back(rel + " (já existe; use --replace para substituir)");
                continue;
            }
        }
        if (auto st = win::writeFileAtomically(win::joinPath(temp, f.relativePath), f.content); !st)
            return st.error();
        report.written.push_back(rel);
        // Só os cofres citados na própria tarefa; sem "vaults", valem os
        // padrões do synkro.yml que estiver em uso.
        try {
            const YAML::Node doc = YAML::Load(f.content);
            if (doc["vaults"] && doc["vaults"].IsSequence())
                for (const auto& v : doc["vaults"])
                    vaultsUsed.insert(v.as<std::string>(""));
        } catch (const YAML::Exception&) {
            // erro de sintaxe aparece na validação abaixo
        }
    }

    // synkro.yml: o desta máquina é mantido; sem ele, nasce do pacote.
    if (!hasGlobal) {
        std::set<std::string> required = vaultsUsed;
        required.insert(pkgGlobal.defaultVaults.begin(), pkgGlobal.defaultVaults.end());
        std::vector<std::string> unbound;
        std::vector<VaultConfig> kept;
        for (auto& v : pkgGlobal.vaults) {
            if (bound.count(v.id)) {
                bindingFor(v, opt.bindings.at(v.id));
                kept.push_back(v);
            } else if (required.count(v.id)) {
                unbound.push_back(v.id + " (\"" + v.label + "\")");
            } else {
                report.warnings.push_back("cofre \"" + v.label + "\" não é usado pelas tarefas importadas e ficou de fora");
            }
        }
        pkgGlobal.vaults = kept;
        if (!unbound.empty()) {
            std::string list;
            for (const auto& u : unbound)
                list += (list.empty() ? "" : ", ") + u;
            return Error::make(ErrorCode::InvalidArgument,
                               "ligue cada cofre do pacote a um disco desta máquina com --bind <id>=<disco>: " + list);
        }
        if (auto st = win::writeFileAtomically(win::joinPath(temp, L"synkro.yml"), emitGlobal(pkgGlobal)); !st)
            return st.error();
        report.written.insert(report.written.begin(), "synkro.yml");
    } else {
        std::vector<Diagnostic> d;
        GlobalConfig mine = parseGlobalDocument(text(targetGlobal), L"synkro.yml", d);
        std::vector<VaultConfig> toAdd;
        std::vector<std::string> missing;
        for (const auto& id : vaultsUsed) {
            if (mine.findVault(id))
                continue;
            const VaultConfig* role = pkgGlobal.findVault(id);
            if (!role || !bound.count(id)) {
                missing.push_back(id);
                continue;
            }
            VaultConfig v = *role;
            bindingFor(v, opt.bindings.at(id));
            toAdd.push_back(v);
        }
        if (!missing.empty()) {
            std::string list;
            for (const auto& m : missing)
                list += (list.empty() ? "" : ", ") + m;
            return Error::make(ErrorCode::InvalidArgument,
                               "as tarefas importadas usam cofres que não existem nesta máquina: " + list
                                   + "; ligue-os com --bind <id>=<disco>");
        }
        if (!toAdd.empty()) {
            if (!isPristineGenerated(text(targetGlobal)))
                return Error::make(ErrorCode::InvalidArgument,
                                   "o synkro.yml desta máquina foi editado à mão; acrescente os cofres "
                                       + [&] {
                                             std::string l;
                                             for (const auto& v : toAdd)
                                                 l += (l.empty() ? "" : ", ") + v.id;
                                             return l;
                                         }()
                                       + " nele e importe de novo (o Synkro não regrava arquivos editados à mão)");
            for (auto& v : toAdd) {
                mine.vaults.push_back(v);
                report.warnings.push_back("cofre \"" + v.label + "\" acrescentado ao synkro.yml");
            }
            if (auto st = win::writeFileAtomically(win::joinPath(temp, L"synkro.yml"), emitGlobal(mine)); !st)
                return st.error();
            report.written.insert(report.written.begin(), "synkro.yml");
        } else {
            report.kept.push_back("synkro.yml (o desta máquina)");
        }
    }

    // Valida o resultado inteiro antes de tocar na configuração real.
    LoadOptions lo;
    lo.secretsFile = opt.secretsFile;
    lo.checkPhysicalDisks = opt.checkPhysicalDisks;
    ConfigSet result = loadConfig(temp, lo);
    report.diagnostics = result.diagnostics;
    if (result.hasErrors()) {
        std::string first;
        for (const auto& d : result.diagnostics) {
            if (d.severity == Severity::Error) {
                first = formatDiagnostic(d);
                break;
            }
        }
        return Error::make(ErrorCode::InvalidArgument, "a configuração resultante teria " + std::to_string(result.errorCount())
                                                           + " erro(s); nada foi alterado. Primeiro: " + first);
    }

    // Segredos que esta máquina ainda não tem.
    {
        SecretStore store(opt.secretsFile.empty() ? secretsFile() : opt.secretsFile);
        std::set<std::string> have;
        if (auto names = store.names())
            have.insert(names.value().begin(), names.value().end());
        std::set<std::string> needed(pkg.secrets.begin(), pkg.secrets.end());
        for (const auto& j : result.jobs)
            for (const auto& s : j.secrets)
                needed.insert(s.name);
        for (const auto& s : needed)
            if (!have.count(s))
                report.missingSecrets.push_back(s);
    }

    if (opt.dryRun)
        return report;
    if (auto st = win::createDirectories(win::joinPath(opt.configDir, L"jobs")); !st)
        return st.error();
    for (const auto& rel : report.written) {
        const std::wstring r = win::fromUtf8(rel);
        if (auto st = win::writeFileAtomically(win::joinPath(opt.configDir, r), text(win::joinPath(temp, r))); !st)
            return st.error();
    }
    return report;
}

} // namespace synkro::config
