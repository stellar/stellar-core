// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/CryptoError.h"
#include "invariant/InvariantDoesNotHold.h"
#include "ledger/NonSociRelatedException.h"
#include "main/ApplicationUtils.h"
#include "main/CommandLine.h"
#include "main/Config.h"
#include "main/StellarCoreVersion.h"
#include "rust/RustBridge.h"
#include "util/Backtrace.h"
#include "util/FileSystemException.h"
#include "util/Logging.h"
#include <mutex>
#include <regex>
#include <stdexcept>

#include "crypto/ShortHash.h"
#include "util/RandHasher.h"
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <sodium/core.h>
#include <system_error>
#include <xdrpp/marshal.h>
#ifdef USE_TRACY
#include <Tracy.hpp>
#include <TracyC.h>
#endif

namespace stellar
{
static void
printCurrentException()
{
    std::exception_ptr eptr = std::current_exception();
    if (eptr)
    {
        try
        {
            std::rethrow_exception(eptr);
        }
        catch (NonSociRelatedException const& e)
        {
            fprintf(stderr,
                    "current exception: NonSociRelatedException(\"%s\")\n",
                    e.what());
        }
        catch (CryptoError const& e)
        {
            fprintf(stderr, "current exception: CryptoError(\"%s\")\n",
                    e.what());
        }
        catch (FileSystemException const& e)
        {
            fprintf(stderr, "current exception: FileSystemException(\"%s\")\n",
                    e.what());
        }
        catch (InvariantDoesNotHold const& e)
        {
            fprintf(stderr, "current exception: InvariantDoesNotHold(\"%s\")\n",
                    e.what());
        }
        catch (std::filesystem::filesystem_error const& e)
        {
            fprintf(stderr,
                    "current exception: std::filesystem::filesystem_error(%d, "
                    "\"%s\", \"%s\", \"%s\", \"%s\")\n",
                    e.code().value(), e.code().message().c_str(), e.what(),
                    e.path1().string().c_str(), e.path2().string().c_str());
        }
        catch (std::system_error const& e)
        {
            fprintf(
                stderr,
                "current exception: std::system_error(%d, \"%s\", \"%s\")\n",
                e.code().value(), e.code().message().c_str(), e.what());
        }
        catch (std::domain_error const& e)
        {
            fprintf(stderr, "current exception: std::domain_error(\"%s\")\n",
                    e.what());
        }
        catch (std::invalid_argument const& e)
        {
            fprintf(stderr,
                    "current exception: std::invalid_argument(\"%s\")\n",
                    e.what());
        }
        catch (std::length_error const& e)
        {
            fprintf(stderr, "current exception: std::length_error(\"%s\")\n",
                    e.what());
        }
        catch (std::out_of_range const& e)
        {
            fprintf(stderr, "current exception: std::out_of_range(\"%s\")\n",
                    e.what());
        }
        catch (std::range_error const& e)
        {
            fprintf(stderr, "current exception: std::range_error(\"%s\")\n",
                    e.what());
        }
        catch (std::overflow_error const& e)
        {
            fprintf(stderr, "current exception: std::overflow_error(\"%s\")\n",
                    e.what());
        }
        catch (std::underflow_error const& e)
        {
            fprintf(stderr, "current exception: std::underflow_error(\"%s\")\n",
                    e.what());
        }
        catch (std::logic_error const& e)
        {
            fprintf(stderr, "current exception: std::logic_error(\"%s\")\n",
                    e.what());
        }
        catch (std::runtime_error const& e)
        {
            fprintf(stderr, "current exception: std::runtime_error(\"%s\")\n",
                    e.what());
        }
        catch (std::exception const& e)
        {
            fprintf(stderr, "current exception: std::exception(\"%s\")\n",
                    e.what());
        }
        catch (...)
        {
            fprintf(stderr, "current exception: unknown\n");
        }
        fflush(stderr);
    }
}

static void
printBacktraceAndAbort()
{
    printCurrentException();
    printCurrentBacktrace();
    std::abort();
}

static void
outOfMemory()
{
    std::fprintf(stderr, "Unable to allocate memory\n");
    std::fflush(stderr);
    printBacktraceAndAbort();
}
}

// We would like this to be a static check but it seems like cxx.rs isn't going
// to let us export static constants so we do it first thing during startup.
//
// The file hashes used by the C++ side are defined in a build-system-generated
// file XDRFilesSha256.cpp. We declare this symbol here and check it against the
// Rust hashes in checkXDRFileIdentity.
namespace stellar
{
extern const std::vector<std::pair<std::filesystem::path, std::string>>
    XDR_FILES_SHA256;
}

void
checkXDRFileIdentity()
{
    using namespace stellar::rust_bridge;

    // This will panic if soroban does not support the current ledger protocol
    // version. It should even work if configured with "next": the next feature
    // should enable the next feature on the most recent soroban host, and to
    // select the next xdr module from the xdr crate linked to that host.
    rust::Vec<SorobanVersionInfo> rustVersions = get_soroban_version_info(
        stellar::Config::CURRENT_LEDGER_PROTOCOL_VERSION);
    rust::Vec<XDRFileHash> const& rustHashes =
        rustVersions.back().xdr_file_hashes;

    for (auto const& cpp : stellar::XDR_FILES_SHA256)
    {
        if (cpp.first.empty())
        {
            continue;
        }
        bool found = false;
        for (auto const& rust : rustHashes)
        {
            std::filesystem::path rustPath(
                std::string(rust.file.cbegin(), rust.file.cend()));
            if (rustPath.filename() == cpp.first.filename())
            {
                std::string rustHash(rust.hash.begin(), rust.hash.end());
                if (rustHash == cpp.second)
                {
                    found = true;
                    break;
                }
                else
                {
                    throw std::runtime_error(fmt::format(
                        "XDR hash mismatch: rust has {}={}, C++ has {}={}",
                        rustPath, rustHash, cpp.first, cpp.second));
                }
            }
        }
        if (!found)
        {
            throw std::runtime_error(
                fmt::format("XDR hash missing: C++ has {}={} with no "
                            "corresponding Rust file",
                            cpp.first, cpp.second));
        }
    }

    if (stellar::XDR_FILES_SHA256.size() != rustHashes.size())
    {
        throw std::runtime_error(
            fmt::format("Number of xdr hashes don't match between C++ and "
                        "Rust. C++ size = {} and Rust size = {}.",
                        stellar::XDR_FILES_SHA256.size(), rustHashes.size()));
    }
}

void
checkStellarCoreMajorVersionProtocolIdentity()
{
    // This extracts a major version number from the git version string embedded
    // in the binary if, and only if, that version string has the form of a
    // release tag: specifically vX.Y.Z, or vX.Y.ZrcN, or vX.Y.ZHOTN. Other
    // version strings return nullopt, for example non-release-tagged versions
    // that typically look more like `v21.0.0rc1-84-g08d89bb4a`
    auto major_release_version =
        stellar::getStellarCoreMajorReleaseVersion(STELLAR_CORE_VERSION);
    if (major_release_version)
    {
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
        // In a vNext build, we expect the major release version to be one less
        // than the CURRENT_LEDGER_PROTOCOL_VERSION. In other words if we are
        // developing v21.X.Y and we enable vNext, then
        // CURRENT_LEDGER_PROTOCOL_VERSION should be 22.
        if (*major_release_version + 1 !=
            stellar::Config::CURRENT_LEDGER_PROTOCOL_VERSION)
        {
            throw std::runtime_error(
                fmt::format("stellar-core version {} has major version {} and "
                            "is configured for next-protocol support, but "
                            "CURRENT_LEDGER_PROTOCOL_VERSION is {}",
                            STELLAR_CORE_VERSION, *major_release_version,
                            stellar::Config::CURRENT_LEDGER_PROTOCOL_VERSION));
        }
#else
        // In a non-vNext build, we expect the major release version to be the
        // same as the CURRENT_LEDGER_PROTOCOL_VERSION. In other words if we are
        // developing v21.X.Y and we are not enabling vNext, then
        // CURRENT_LEDGER_PROTOCOL_VERSION should be 21.
        if (*major_release_version !=
            stellar::Config::CURRENT_LEDGER_PROTOCOL_VERSION)
        {
            throw std::runtime_error(
                fmt::format("stellar-core version {} has major version {} but "
                            "CURRENT_LEDGER_PROTOCOL_VERSION is {}",
                            STELLAR_CORE_VERSION, *major_release_version,
                            stellar::Config::CURRENT_LEDGER_PROTOCOL_VERSION));
        }
#endif
    }
    else
    {
        // If we are running a version that does not look exactly like vX.Y.Z or
        // vX.Y.ZrcN or vX.Y.ZHOTN, then we are running a non-release version of
        // stellar-core and we relax the check above and just warn.
        std::cerr << "Warning: running non-release version "
                  << STELLAR_CORE_VERSION << " of stellar-core" << std::endl;
    }
}

#ifdef USE_TRACY_MEMORY_TRACKING

#ifdef __has_feature
#if __has_feature(address_sanitizer)
#define ASAN_ENABLED
#endif
#else
#ifdef __SANITIZE_ADDRESS__
#define ASAN_ENABLED
#endif
#endif

#ifdef ASAN_ENABLED
#error "ASAN_ENABLED and USE_TRACY_MEMORY_TRACKING are mutually exclusive"
#else
void*
operator new(std::size_t count)
{
    auto ptr = malloc(count);
    // "Secure" here means "tolerant of calls outside the
    // lifeitme of the tracy client".
    TracySecureAlloc(ptr, count);
    return ptr;
}

void
operator delete(void* ptr) noexcept
{
    TracySecureFree(ptr);
    free(ptr);
}

void*
operator new[](std::size_t count)
{
    auto ptr = malloc(count);
    TracySecureAlloc(ptr, count);
    return ptr;
}

void
operator delete[](void* ptr) noexcept
{
    TracySecureFree(ptr);
    free(ptr);
}
#endif // !ASAN_ENABLED
#endif // USE_TRACY_MEMORY_TRACKING

int
main(int argc, char* const* argv)
{
    using namespace stellar;
    BacktraceManager btGuard;

    // Abort when out of memory
    std::set_new_handler(outOfMemory);
    // At least print a backtrace in any circumstance
    // that would call std::terminate
    std::set_terminate(printBacktraceAndAbort);
    Logging::init();
    if (sodium_init() != 0)
    {
        LOG_FATAL(DEFAULT_LOG, "Could not initialize crypto");
        return 1;
    }
    initializeAllGlobalState();
    xdr::marshaling_stack_limit = 1000;

    checkStellarCoreMajorVersionProtocolIdentity();
    rust_bridge::check_sensible_soroban_config_for_protocol(
        Config::CURRENT_LEDGER_PROTOCOL_VERSION);
    checkXDRFileIdentity();

    int res = handleCommandLine(argc, argv);
    return res;
}
