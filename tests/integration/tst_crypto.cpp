// Cofre cifrado: primitivas (vetores conhecidos), formato de arquivo,
// adulteração, chave de recuperação e o ciclo completo de backup,
// verificação e restauração num cofre cifrado.

#include "crypto/Crypto.h"
#include "crypto/FileCipher.h"
#include "crypto/VaultKey.h"
#include "engine/BackupEngine.h"
#include "engine/Restore.h"
#include "engine/VaultOps.h"
#include "engine/Verify.h"
#include "secrets/SecretStore.h"
#include "vault/JobLayout.h"
#include "vault/Vault.h"

#include "TestFs.h"

#include <QTemporaryDir>
#include <QTest>

using namespace synkro;
using namespace synkro::test;

namespace {

std::string hexOf(const std::uint8_t* p, std::size_t n)
{
    static constexpr char d[] = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

std::string pattern(std::size_t n)
{
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i)
        s[i] = static_cast<char>((i * 131u + 7u) & 0xFF);
    return s;
}

std::shared_ptr<const crypto::FileCipher> cipherWith(std::uint8_t fill)
{
    crypto::Key k{};
    k.fill(fill);
    auto c = crypto::FileCipher::fromKey(k);
    return c ? c.value() : nullptr;
}

struct VaultEnv {
    QTemporaryDir dir;
    std::wstring root = nativePath(dir.path());
    std::wstring source = win::joinPath(root, L"origem");
    std::wstring vault = win::joinPath(root, L"cofre");
    std::wstring secrets = win::joinPath(root, L"secrets.dat");

    VaultEnv()
    {
        (void)win::createDirectories(source);
        (void)win::createDirectories(vault);
        (void)vault::initialize(vault, QStringLiteral("Cofre cifrado"));
        crypto::forgetRememberedKeys();
    }

    BackupRequest request() const
    {
        BackupRequest r;
        r.jobId = L"pdv";
        r.sourceRoot = source;
        r.vaults.push_back({vault, {}, QStringLiteral("Cofre cifrado")});
        r.applyRetention = false;
        r.secretsFile = secrets;
        return r;
    }
};

} // namespace

class TestCrypto : public QObject {
    Q_OBJECT

private slots:
    void knownAnswers()
    {
        // AES-256-GCM, caso de teste 14 da especificação do GCM (McGrew & Viega).
        crypto::Key zero{};
        auto aes = crypto::AesGcm::create(zero);
        QVERIFY(aes);
        crypto::Nonce nonce{};
        std::uint8_t plain[16] = {}, out[16] = {};
        crypto::Tag tag{};
        QVERIFY(aes.value().encrypt(nonce, {}, plain, 16, out, tag));
        QCOMPARE(hexOf(out, 16), std::string("cea7403d4d606b6e074ec5d3baf39d18"));
        QCOMPARE(hexOf(tag.data(), 16), std::string("d0d1c8a799996bf0265b98b5d48ab919"));
        std::uint8_t back[16] = {};
        QVERIFY(aes.value().decrypt(nonce, {}, out, 16, back, tag));
        QCOMPARE(hexOf(back, 16), std::string(32, '0'));
        tag[0] ^= 1;
        const auto bad = aes.value().decrypt(nonce, {}, out, 16, back, tag);
        QVERIFY(!bad);
        QCOMPARE(bad.error().code, ErrorCode::Corrupted);

        // PBKDF2-HMAC-SHA256 ("password", "salt", 1 iteração).
        auto k = crypto::pbkdf2Sha256("password", "salt", 1);
        QVERIFY(k);
        QCOMPARE(hexOf(k.value().data(), 32), std::string("120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"));
    }

    void streamRoundTripSizes()
    {
        auto c = cipherWith(7);
        QVERIFY(c);
        const std::size_t chunk = crypto::kChunkSize;
        for (std::size_t n : {std::size_t(0), std::size_t(1), chunk - 1, chunk, chunk + 1, 3 * chunk + 5}) {
            const std::string plain = pattern(n);
            auto enc = c->encryptBytes(plain);
            QVERIFY(enc);
            const std::size_t chunks = n == 0 ? 1 : (n + chunk - 1) / chunk;
            QCOMPARE(enc.value().size(), crypto::kHeaderSize + n + chunks * crypto::kTagSize);
            QVERIFY(enc.value().compare(0, 8, "SYNKENC1") == 0);
            if (n > 64)
                QVERIFY(enc.value().find(plain.substr(0, 64)) == std::string::npos); // nada em claro
            auto dec = c->decryptBytes(enc.value());
            QVERIFY2(dec, dec ? "" : dec.error().message.c_str());
            QVERIFY(dec.value() == plain);
        }
        // Mesmo conteúdo, salt diferente: cifrados diferentes.
        QVERIFY(c->encryptBytes("abc").value() != c->encryptBytes("abc").value());
    }

    void tamperingIsDetected()
    {
        auto c = cipherWith(9);
        const std::string plain = pattern(2 * crypto::kChunkSize + 100);
        const std::string enc = c->encryptBytes(plain).value();

        std::string flipped = enc;
        flipped[crypto::kHeaderSize + 10] ^= 0x40;
        QVERIFY(!c->decryptBytes(flipped));

        std::string header = enc;
        header[20] ^= 1; // salt (autenticado como dado adicional)
        QVERIFY(!c->decryptBytes(header));

        // Truncado num limite de bloco: o último bloco que sobrou não tem a marca de final.
        const std::size_t block = crypto::kChunkSize + crypto::kTagSize;
        QVERIFY(!c->decryptBytes(enc.substr(0, crypto::kHeaderSize + 2 * block)));
        QVERIFY(!c->decryptBytes(enc.substr(0, crypto::kHeaderSize + block)));
        QVERIFY(!c->decryptBytes(enc.substr(0, enc.size() - 1)));

        // Blocos trocados de ordem.
        std::string swapped = enc.substr(0, crypto::kHeaderSize) + enc.substr(crypto::kHeaderSize + block, block)
                              + enc.substr(crypto::kHeaderSize, block) + enc.substr(crypto::kHeaderSize + 2 * block);
        QVERIFY(!c->decryptBytes(swapped));

        // Chave de outro cofre: recusada já no cabeçalho, com mensagem clara.
        auto other = cipherWith(10);
        auto wrong = other->decryptBytes(enc);
        QVERIFY(!wrong);
        QVERIFY(wrong.error().message.find("outra chave") != std::string::npos);
    }

    void recoveryKeyFormat()
    {
        crypto::RecoveryBytes b{};
        for (std::size_t i = 0; i < b.size(); ++i)
            b[i] = static_cast<std::uint8_t>(i * 37 + 1);
        const std::string text = crypto::formatRecoveryKey(b);
        QCOMPARE(text.size(), std::size_t(39)); // 8 grupos de 4 + 7 hífens
        QCOMPARE(std::count(text.begin(), text.end(), '-'), std::ptrdiff_t(7));
        auto back = crypto::parseRecoveryKey(text);
        QVERIFY(back);
        QVERIFY(back.value() == b);

        // Digitação tolerante: minúsculas, espaços, sem hífens, 0/1/8 no lugar de O/I/B.
        std::string typed;
        for (char ch : text) {
            if (ch == '-')
                typed += ' ';
            else
                typed += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        QVERIFY(crypto::parseRecoveryKey(typed));
        std::string withDigits = text;
        for (auto& ch : withDigits)
            ch = ch == 'O' ? '0' : ch == 'I' ? '1' : ch == 'B' ? '8' : ch;
        QVERIFY(crypto::parseRecoveryKey(withDigits).value() == b);
        QVERIFY(!crypto::parseRecoveryKey("ABCD-EFGH"));
    }

    void encryptedVaultLifecycle()
    {
        VaultEnv env;
        QVERIFY(writeFile(win::joinPath(env.source, L"NFe\\nota1.xml"), "<nfe><cpf>123.456.789-00</cpf></nfe>"));
        QVERIFY(writeFile(win::joinPath(env.source, L"pdv.ini"), QByteArray::fromStdString(pattern(3 * crypto::kChunkSize + 9))));

        auto enabled = crypto::enableEncryption(env.vault, env.secrets);
        QVERIFY2(enabled, enabled ? "" : enabled.error().message.c_str());
        QVERIFY(enabled.value().storedOnMachine);
        const std::string recovery = enabled.value().recoveryKey;
        QVERIFY(crypto::isEncrypted(env.vault));
        QVERIFY(!crypto::enableEncryption(env.vault, env.secrets)); // já cifrado

        // Backup: o worker usa a chave guardada nesta máquina.
        crypto::forgetRememberedKeys();
        auto r = runBackup(env.request());
        QVERIFY2(r.status == RunStatus::Success, (r.error + (r.vaults.empty() ? "" : " | " + r.vaults[0].error)).c_str());
        const vault::JobLayout layout(env.vault, L"pdv");
        const std::wstring stored = layout.currentFile(L"NFe/nota1.xml");
        QVERIFY(crypto::looksEncrypted(stored));
        QVERIFY(!readFile(stored).contains("123.456.789-00")); // o dado sensível não está em claro

        // Verificação completa (decifra e confere o SHA-256 do original).
        auto v = verifyVault(layout, 100.0, env.secrets);
        QVERIFY2(v, v ? "" : v.error().message.c_str());
        QCOMPARE(v.value().ok, std::size_t(2));

        // Restauração com a chave da máquina.
        RestoreRequest rr;
        rr.vaultLocation = env.vault;
        rr.jobId = L"pdv";
        rr.targetDirectory = win::joinPath(env.root, L"restaurado");
        rr.secretsFile = env.secrets;
        auto restored = restore(rr);
        QVERIFY2(restored, restored ? "" : restored.error().message.c_str());
        QCOMPARE(restored.value().restored, std::size_t(2));
        QVERIFY(restored.value().failures.empty());
        QCOMPARE(readFile(win::joinPath(rr.targetDirectory, L"NFe\\nota1.xml")),
                 QByteArray("<nfe><cpf>123.456.789-00</cpf></nfe>"));

        // Outro computador (sem a chave): restauração pede a chave de recuperação.
        crypto::forgetRememberedKeys();
        const std::wstring otherSecrets = win::joinPath(env.root, L"outro-pc-secrets.dat");
        rr.secretsFile = otherSecrets;
        rr.targetDirectory = win::joinPath(env.root, L"restaurado-outro-pc");
        auto blocked = restore(rr);
        QVERIFY(!blocked);
        QCOMPARE(blocked.error().code, ErrorCode::KeyRequired);

        auto wrong = crypto::unlockWithRecoveryKey(env.vault, "AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AAAA");
        QVERIFY(!wrong);
        QCOMPARE(wrong.error().code, ErrorCode::AccessDenied);
        auto key = crypto::unlockWithRecoveryKey(env.vault, recovery);
        QVERIFY2(key, key ? "" : key.error().message.c_str());
        auto id = vault::readIdentity(env.vault);
        crypto::rememberKey(id.value().vaultId, key.value());
        auto again = restore(rr);
        QVERIFY2(again, again ? "" : again.error().message.c_str());
        QCOMPARE(again.value().restored, std::size_t(2));
        QCOMPARE(readFile(win::joinPath(rr.targetDirectory, L"pdv.ini")),
                 QByteArray::fromStdString(pattern(3 * crypto::kChunkSize + 9)));

        // Adulteração no cofre: a verificação acusa corrupção.
        {
            QByteArray bytes = readFile(stored);
            bytes[static_cast<qsizetype>(crypto::kHeaderSize + 3)] ^= 0x01;
            QVERIFY(writeFile(stored, bytes));
        }
        auto v2 = verifyVault(layout, 100.0, env.secrets);
        QVERIFY(v2);
        QCOMPARE(v2.value().corrupted.size(), std::size_t(1));

        // key.json apagado: esta máquina sabe que o cofre era cifrado e não grava em claro.
        QVERIFY(::DeleteFileW(win::toExtendedPath(crypto::keyFilePath(env.vault)).c_str()));
        crypto::forgetRememberedKeys();
        auto r2 = runBackup(env.request());
        QVERIFY(r2.status != RunStatus::Success);
        QVERIFY(r2.vaults[0].error.find("key.json") != std::string::npos);
    }

    void cannotEncryptVaultWithData()
    {
        VaultEnv env;
        QVERIFY(writeFile(win::joinPath(env.source, L"a.txt"), "x"));
        auto r = runBackup(env.request());
        QVERIFY(r.status == RunStatus::Success);
        auto enabled = crypto::enableEncryption(env.vault, env.secrets);
        QVERIFY(!enabled);
        QVERIFY(enabled.error().message.find("cofre novo") != std::string::npos);
    }

    void missingKeyBlocksBackup()
    {
        VaultEnv env;
        QVERIFY(writeFile(win::joinPath(env.source, L"a.txt"), "x"));
        QVERIFY(crypto::enableEncryption(env.vault, win::joinPath(env.root, L"pc-que-cifrou.dat")));
        crypto::forgetRememberedKeys();
        auto r = runBackup(env.request()); // env.secrets não tem a chave
        QVERIFY(r.status != RunStatus::Success);
        QVERIFY(r.vaults[0].error.find("chave de recuperação") != std::string::npos);
        // Nada foi gravado em claro.
        QVERIFY(!ops::pathExists(vault::JobLayout(env.vault, L"pdv").currentFile(L"a.txt")));
    }
};

QTEST_GUILESS_MAIN(TestCrypto)
#include "tst_crypto.moc"
