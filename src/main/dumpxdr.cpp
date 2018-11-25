#include "main/dumpxdr.h"
#include "crypto/SecretKey.h"
#include "transactions/SignatureUtils.h"
#include "util/Decoder.h"
#include "util/Fs.h"
#include "util/XDROperators.h"
#include "util/XDRStream.h"
#include "util/format.h"
#include <iostream>
#include <regex>
#include <xdrpp/printer.h>

#if !defined(USE_TERMIOS) && !MSVC
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

#if MSVC
#include <io.h>
#define isatty _isatty
#endif // MSVC

using namespace std::placeholders;

namespace stellar
{

const char* signtxn_network_id;

std::string
xdr_printer(const PublicKey& pk)
{
    return KeyUtils::toStrKey<PublicKey>(pk);
}

template <typename T>
void
dumpstream(XDRInputFileStream& in)
{
    T tmp;
    while (in && in.readOne(tmp))
    {
        std::cout << xdr::xdr_to_string(tmp) << std::endl;
    }
}

void
dumpXdrStream(std::string const& filename)
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
            dumpstream<LedgerHeaderHistoryEntry>(in);
        }
        else if (sm[1] == "bucket")
        {
            dumpstream<BucketEntry>(in);
        }
        else if (sm[1] == "transactions")
        {
            dumpstream<TransactionHistoryEntry>(in);
        }
        else if (sm[1] == "results")
        {
            dumpstream<TransactionHistoryResultEntry>(in);
        }
        else
        {
            assert(sm[1] == "scp");
            dumpstream<SCPHistoryEntry>(in);
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
    if (filename == "-" || filename.empty())
        input << cin.rdbuf();
    else
    {
        ifstream file(filename.c_str());
        if (!file)
            throw_perror(filename);
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
printOneXdr(xdr::opaque_vec<> const& o, std::string const& desc)
{
    T tmp;
    xdr::xdr_from_opaque(o, tmp);
    std::cout << xdr::xdr_to_string(tmp, desc.c_str()) << std::endl;
}

void
printXdr(std::string const& filename, std::string const& filetype, bool base64)
{
// need to use this pattern as there is no good way to get a human readable
// type name from a type
#define PRINTONEXDR(T) std::bind(printOneXdr<T>, _1, #T)
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
#if !HAVE_TERMIOS
    // Sorry, Windows.  Might need to use something like this:
    // https://msdn.microsoft.com/en-us/library/ms683167(VS.85).aspx
    // http://stackoverflow.com/questions/1413445/read-a-password-from-stdcin/1455007#1455007
    throw std::invalid_argument("reading secrets from terminal not supported");
#else
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
signtxn(std::string const& filename, bool base64)
{
    using namespace std;

    try
    {
        if (!signtxn_network_id)
            signtxn_network_id = getenv("STELLAR_NETWORK_ID");
        if (!signtxn_network_id)
            throw std::runtime_error("missing --netid argument or "
                                     "STELLAR_NETWORK_ID environment variable");

        const bool txn_stdin = filename == "-" || filename.empty();

        if (!base64 && isatty(1))
            throw std::runtime_error(
                "Refusing to write binary transaction to terminal");

        TransactionEnvelope txenv;
        xdr::xdr_from_opaque(readFile(filename, base64), txenv);
        if (txenv.signatures.size() == txenv.signatures.max_size())
            throw std::runtime_error(
                "Evelope already contains maximum number of signatures");

        SecretKey sk(SecretKey::fromStrKeySeed(
            readSecret("Secret key seed: ", txn_stdin)));
        TransactionSignaturePayload payload;
        payload.networkId = sha256(std::string(signtxn_network_id));
        payload.taggedTransaction.type(ENVELOPE_TYPE_TX);
        payload.taggedTransaction.tx() = txenv.tx;
        txenv.signatures.emplace_back(
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
