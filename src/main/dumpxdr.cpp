#include "main/dumpxdr.h"
#include "crypto/Hex.h"
#include "crypto/SecretKey.h"
#include "crypto/StrKey.h"
#include "main/Config.h"
#include "transactions/SignatureUtils.h"
#include "transactions/TransactionBridge.h"
#include "transactions/TransactionUtils.h"
#include "util/Decoder.h"
#include "util/Fs.h"
#include "util/XDRCereal.h"
#include "util/XDROperators.h"
#include "util/XDRStream.h"
#include "util/types.h"
#include <cereal/archives/json.hpp>
#include <cereal/cereal.hpp>
#include <fmt/format.h>
#include <iostream>
#include <regex>
#include <xdrpp/printer.h>

#if !defined(USE_TERMIOS) && !defined(_WIN32)
#define HAVE_TERMIOS 1
#endif
#if HAVE_TERMIOS
extern "C"
{
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
}
#endif // HAVE_TERMIOS

#ifdef _WIN32
#include <io.h>
#define isatty _isatty
#include <wincred.h>
#endif // _WIN32

using namespace std::placeholders;

namespace stellar
{

template <typename T>
void
dumpstream(XDRInputFileStream& in, bool compact)
{
    T tmp;
    cereal::JSONOutputArchive archive(
        std::cout, compact ? cereal::JSONOutputArchive::Options::NoIndent()
                           : cereal::JSONOutputArchive::Options::Default());
    archive.makeArray();
    while (in && in.readOne(tmp))
    {
        archive(tmp);
    }
}

void
dumpXdrStream(std::string const& filename, bool compact)
{
    std::regex rx(
        ".*(ledger|bucket|transactions|results|scp)-[[:xdigit:]]+\\.xdr");
    std::smatch sm;
    if (std::regex_match(filename, sm, rx))
    {
        XDRInputFileStream in;
        in.open(filename);

        if (sm[1] == "ledger")
        {
            dumpstream<LedgerHeaderHistoryEntry>(in, compact);
        }
        else if (sm[1] == "bucket")
        {
            dumpstream<BucketEntry>(in, compact);
        }
        else if (sm[1] == "transactions")
        {
            dumpstream<TransactionHistoryEntry>(in, compact);
        }
        else if (sm[1] == "results")
        {
            dumpstream<TransactionHistoryResultEntry>(in, compact);
        }
        else
        {
            assert(sm[1] == "scp");
            dumpstream<SCPHistoryEntry>(in, compact);
        }
    }
    else
    {
        throw std::runtime_error("unrecognized XDR filename");
    }
}

#define throw_perror(msg) \
    do \
    { \
        throw std::runtime_error(std::string(msg) + ": " + \
                                 xdr_strerror(errno)); \
    } while (0)

static xdr::opaque_vec<>
readFile(const std::string& filename, bool base64 = false)
{
    using namespace std;
    ostringstream input;
    if (filename == Config::STDIN_SPECIAL_NAME || filename.empty())
        input << cin.rdbuf();
    else
    {
        ifstream file(filename.c_str());
        if (!file)
        {
            throw_perror(filename);
        }
        file.exceptions(std::ios::badbit);
        input << file.rdbuf();
    }
    string ret;
    if (base64)
        decoder::decode_b64(input.str(), ret);
    else
        ret = input.str();
    return {ret.begin(), ret.end()};
}

template <typename T>
void
printOneXdr(xdr::opaque_vec<> const& o, std::string const& desc, bool compact)
{
    T tmp;
    xdr::xdr_from_opaque(o, tmp);
    std::cout << xdr_to_string(tmp, desc, compact) << std::endl;
}

void
printXdr(std::string const& filename, std::string const& filetype, bool base64,
         bool compact)
{
// need to use this pattern as there is no good way to get a human readable
// type name from a type
#define PRINTONEXDR(T) std::bind(printOneXdr<T>, _1, #T, compact)
    auto dumpMap =
        std::map<std::string, std::function<void(xdr::opaque_vec<> const&)>>{
            {"ledgerheader", PRINTONEXDR(LedgerHeader)},
            {"meta", PRINTONEXDR(TransactionMeta)},
            {"result", PRINTONEXDR(TransactionResult)},
            {"resultpair", PRINTONEXDR(TransactionResultPair)},
            {"tx", PRINTONEXDR(TransactionEnvelope)},
            {"txfee", PRINTONEXDR(LedgerEntryChanges)}};
#undef PRINTONEXDR

    try
    {
        auto d = readFile(filename, base64);

        if (filetype == "auto")
        {
            bool processed = false;
            for (auto const& it : dumpMap)
            {
                try
                {
                    it.second(d);
                    processed = true;
                    break;
                }
                catch (xdr::xdr_runtime_error)
                {
                }
            }
            if (!processed)
            {
                throw std::invalid_argument("Could not detect type");
            }
        }
        else
        {
            auto it = dumpMap.find(filetype);
            if (it != dumpMap.end())
            {
                it->second(d);
            }
            else
            {
                throw std::invalid_argument(
                    fmt::format("unknown filetype {}", filetype));
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "Could not decode with type '" << filetype
                  << "' : " << e.what() << std::endl;
    }
}

#if HAVE_TERMIOS
static int
set_echo_flag(int fd, bool flag)
{
    struct termios tios;
    if (tcgetattr(fd, &tios))
        return -1;
    int old = !!(tios.c_lflag & ECHO);
    if (flag)
        tios.c_lflag |= ECHO;
    else
        tios.c_lflag &= ~ECHO;
    return tcsetattr(fd, TCSAFLUSH, &tios) == 0 ? old : -1;
}
#endif

#ifdef _WIN32
static std::string
getSecureCreds(std::string const& prompt)
{
    std::string res;
    CREDUI_INFO cui;
    TCHAR pszName[CREDUI_MAX_USERNAME_LENGTH + 1] = TEXT("default");
    TCHAR pszPwd[CREDUI_MAX_PASSWORD_LENGTH + 1];
    BOOL fSave;
    DWORD dwErr;

    cui.cbSize = sizeof(CREDUI_INFO);
    cui.hwndParent = NULL;
    cui.pszMessageText = prompt.c_str();
    cui.pszCaptionText = TEXT("SignerUI");
    cui.hbmBanner = NULL;
    fSave = FALSE;
    SecureZeroMemory(pszPwd, sizeof(pszPwd));
    dwErr = CredUIPromptForCredentials(
        &cui,                              // CREDUI_INFO structure
        TEXT("Stellar"),                   // Target for credentials
                                           //   (usually a server)
        NULL,                              // Reserved
        0,                                 // Reason
        pszName,                           // User name
        CREDUI_MAX_USERNAME_LENGTH + 1,    // Max number of char for user name
        pszPwd,                            // Password
        CREDUI_MAX_PASSWORD_LENGTH + 1,    // Max number of char for password
        &fSave,                            // State of save check box
        CREDUI_FLAGS_GENERIC_CREDENTIALS | // flags
            CREDUI_FLAGS_ALWAYS_SHOW_UI | CREDUI_FLAGS_DO_NOT_PERSIST);

    if (!dwErr)
    {
        res = pszPwd;
    }
    else
    {
        throw std::runtime_error("Could not get password");
    }
    return res;
}

#endif

#if __GNUC__ >= 4 && __GLIBC__ >= 2
// Realistically, if a write fails in one of these utility functions,
// it should kill us with SIGPIPE.  Hence, we don't need to check the
// return value of write.  However, GCC and glibc are now extremely
// aggressive with warn_unused_result, to the point that a cast to
// void won't do anything.  To work around the problem, we define an
// equivalent function without the warn_unused_result attribute.
constexpr ssize_t (&mywrite)(int, const void*, size_t) = ::write;
#else // not (gcc 4+ and glibc)
#define mywrite write
#endif // not (gcc 4+ and glibc)

std::string
readSecret(const std::string& prompt, bool force_tty)
{
    std::string ret;

    if (!isatty(0) && !force_tty)
    {
        std::getline(std::cin, ret);
        return ret;
    }
#ifdef _WIN32
    return getSecureCreds(prompt);
#elif HAVE_TERMIOS
    struct cleanup
    {
        std::function<void()> action_;
        ~cleanup()
        {
            if (action_)
                action_();
        }
    };

    int fd = open("/dev/tty", O_RDWR);
    if (fd == -1)
        throw_perror("/dev/tty");
    cleanup clean_fd;
    clean_fd.action_ = [fd] { close(fd); };

    cleanup clean_echo;
    int oldecho = set_echo_flag(fd, false);
    if (oldecho == -1)
        throw_perror("cannot disable terminal echo");
    else if (oldecho)
        clean_echo.action_ = [fd] { set_echo_flag(fd, true); };

    mywrite(fd, prompt.c_str(), prompt.size());

    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n < 0)
        throw_perror("read secret key");
    mywrite(fd, "\n", 1);
    char* p = static_cast<char*>(std::memchr(buf, '\n', sizeof(buf)));
    if (!p)
        throw std::runtime_error("line too long");
    ret.assign(buf, p - buf);
    memset(buf, 0, sizeof(buf));
    return ret;
#endif
}

void
signtxn(std::string const& filename, std::string netId, bool base64)
{
    using namespace std;

    try
    {
        if (netId.empty())
            netId = getenv("STELLAR_NETWORK_ID");
        if (netId.empty())
            throw std::runtime_error("missing --netid argument or "
                                     "STELLAR_NETWORK_ID environment variable");

        const bool txn_stdin =
            filename == Config::STDIN_SPECIAL_NAME || filename.empty();

        if (!base64 && isatty(1))
            throw std::runtime_error(
                "Refusing to write binary transaction to terminal");

        TransactionEnvelope txenv;
        xdr::xdr_from_opaque(readFile(filename, base64), txenv);
        auto& signatures = txbridge::getSignatures(txenv);
        if (signatures.size() == signatures.max_size())
            throw std::runtime_error(
                "Evelope already contains maximum number of signatures");

        SecretKey sk(SecretKey::fromStrKeySeed(readSecret(
            fmt::format("Secret key seed [network id: '{}']: ", netId),
            txn_stdin)));
        TransactionSignaturePayload payload;
        payload.networkId = sha256(netId);
        switch (txenv.type())
        {
        case ENVELOPE_TYPE_TX_V0:
            payload.taggedTransaction.type(ENVELOPE_TYPE_TX);
            // TransactionV0 and Transaction always have the same signatures so
            // there is no reason to check versions here, just always convert to
            // Transaction
            payload.taggedTransaction.tx() =
                txbridge::convertForV13(txenv).v1().tx;
            break;
        case ENVELOPE_TYPE_TX:
            payload.taggedTransaction.type(ENVELOPE_TYPE_TX);
            payload.taggedTransaction.tx() = txenv.v1().tx;
            break;
        case ENVELOPE_TYPE_TX_FEE_BUMP:
            payload.taggedTransaction.type(ENVELOPE_TYPE_TX_FEE_BUMP);
            payload.taggedTransaction.feeBump() = txenv.feeBump().tx;
            break;
        default:
            abort();
        }

        signatures.emplace_back(
            SignatureUtils::getHint(sk.getPublicKey().ed25519()),
            sk.sign(sha256(xdr::xdr_to_opaque(payload))));

        auto out = xdr::xdr_to_opaque(txenv);
        if (base64)
            cout << decoder::encode_b64(out) << std::endl;
        else
            cout.write(reinterpret_cast<char*>(out.data()), out.size());
    }
    catch (const std::exception& e)
    {
        cerr << e.what() << endl;
    }
}

void
priv2pub()
{
    using namespace std;
    try
    {
        SecretKey sk(
            SecretKey::fromStrKeySeed(readSecret("Secret key seed: ", false)));
        cout << sk.getStrKeyPublic() << endl;
    }
    catch (const std::exception& e)
    {
        cerr << e.what() << endl;
    }
}
}
