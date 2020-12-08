// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "util/Backtrace.h"
#include "config.h"
#include "util/GlobalChecks.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace
{

// The abi::__cxa_demangle call in printCurrentBacktrace below uses a buffer
// that must be a _malloc()_ memory-region, so that it can realloc() if the
// region is too small. To avoid calling malloc() in the middle of unwinding
// (which might mess with already-corrupt state) we pre-malloc() a fairly large
// buffer using BacktraceManager, with hope that it is enough for any symbol we
// encounter.
//
// The BacktraceManager class is idempotent and printCurrentBacktrace sets one
// up locally itself, in case nobody has done so yet. This minimizes the number
// of malloc/free calls and ensures we don't leak (except in a memory-allocation
// error path below where .. we really don't know what the right recovery is).
char* demangleBuf = nullptr;
size_t demangleBufSz = 4096;
}

namespace stellar
{

BacktraceManager::BacktraceManager()
{
    if (!demangleBuf)
    {
        demangleBuf = static_cast<char*>(malloc(demangleBufSz));
        if (demangleBuf)
        {
            doFree = true;
        }
        else
        {
            char const* msg = "BacktraceManager failed to malloc";
            fprintf(stderr, "error: %s\n", msg);
            throw std::runtime_error(msg);
        }
    }
}

BacktraceManager::~BacktraceManager()
{
    if (demangleBuf && doFree)
    {
        free(demangleBuf);
        demangleBuf = nullptr;
    }
}
}

#ifdef HAVE_LIBUNWIND

#define UNW_LOCAL_ONLY
#include <cxxabi.h>
#include <libunwind.h>

namespace stellar
{

void
printCurrentBacktrace()
{
    if (!threadIsMain())
    {
        return;
    }

    if (getenv("STELLAR_NO_BACKTRACE") != nullptr)
    {
        return;
    }

    fprintf(stderr, "backtrace:\n");

    unw_context_t ctxt;
    if (unw_getcontext(&ctxt) != 0)
    {
        return;
    }

    unw_cursor_t curs;
    if (unw_init_local(&curs, &ctxt) != 0)
    {
        return;
    }

    char buf[1024];
    unw_word_t off;
    int i = 0;
    BacktraceManager guard;

    while (unw_step(&curs) > 0)
    {
        if (unw_get_proc_name(&curs, buf, sizeof(buf), &off) != UNW_ESUCCESS)
        {
            fprintf(stderr, "  %4d: <failed to get function name>\n", i++);
            continue;
        }
        int res = 0;
        size_t dsz = demangleBufSz;
        char* d = abi::__cxa_demangle(buf, demangleBuf, &dsz, &res);
        switch (res)
        {
        case 0:
            // Demangling succceeded, might or might-not have realloc'ed
            if (d != demangleBuf || dsz != demangleBufSz)
            {
                // Did realloc successfully; save the new realloc'ed
                // pointer and size. BacktraceManager will free it.
                demangleBuf = d;
                demangleBufSz = dsz;
            }
            // Print the demangled buffer.
            fprintf(stderr, "  %4d: %s\n", i++, d);
            break;
        case -1:
            // Memory-allocation failure occurred; not sure what
            // the fate of demangling buffer is, we'll leak and zero
            // them and hope the next iteration can recover.
            demangleBufSz = 0;
            demangleBuf = nullptr;
            // And print the _mangled_ buffer, best we can do.
            fprintf(stderr, "  %4d: %s\n", i++, buf);
            break;
        case -2:
            // Mangled name doesn't follow proper C++ mangling rules,
            // print mangled name and continue.
            fprintf(stderr, "  %4d: %s\n", i++, buf);
            break;
        case -3:
        default:
            // One of the arguments was invalid or some other error
            // that isn't in the spec. Skip this iteration.
            fprintf(stderr, "  %4d: <failed to demangle function name>\n", i++);
            break;
        }
        fflush(stderr);
    }
}
}

#else
namespace stellar
{

void
printCurrentBacktrace()
{
    fprintf(stderr, "backtrace unavailable\n");
    fflush(stderr);
}
}
#endif
