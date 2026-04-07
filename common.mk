# Hand-written file with variables common to all makefiles

AM_CPPFLAGS = -isystem "$(top_srcdir)" -I"$(top_srcdir)/src" -I"$(top_builddir)/src"
AM_CPPFLAGS += $(libsodium_CFLAGS) $(xdrpp_CFLAGS) $(libmedida_CFLAGS)	\
	$(soci_CFLAGS) $(sqlite3_CFLAGS) $(libasio_CFLAGS) $(libtcmalloc_CFLAGS)
AM_CPPFLAGS += -isystem "$(top_srcdir)/lib"             \
	-isystem "$(top_srcdir)/lib/autocheck/include"      \
	-isystem "$(top_srcdir)/lib/cereal/include"         \
	-isystem "$(top_srcdir)/lib/util"                   \
	-isystem "$(top_srcdir)/lib/fmt/include"            \
	-isystem "$(top_srcdir)/lib/soci/src/core"          \
	-isystem "$(top_srcdir)/lib/tracy/public/tracy"     \
	-isystem "$(top_srcdir)/lib/spdlog/include"         \
	-isystem "$(top_srcdir)/rust/src"

if USE_POSTGRES
AM_CPPFLAGS += -DUSE_POSTGRES=1 $(libpq_CFLAGS)
endif # USE_POSTGRES

AM_CPPFLAGS += -I"$(top_builddir)/src/protocol-curr"

# Unconditionally add CEREAL_THREAD_SAFE, we always want it.
AM_CPPFLAGS += -DCEREAL_THREAD_SAFE

# USE_TRACY and tracy_CFLAGS here represent the case of enabling
# tracy at configure-time; but even when it is disabled we want
# its includes in the CPPFLAGS above, so its (disabled) headers
# and zone-definition macros are included in our code (and
# compiled to no-ops).
if USE_TRACY
AM_CPPFLAGS += -DUSE_TRACY $(tracy_CFLAGS)
endif # USE_TRACY

if BUILD_TESTS
AM_CPPFLAGS += -DBUILD_TESTS=1
endif # BUILD_TESTS

if USE_SPDLOG
AM_CPPFLAGS += -DUSE_SPDLOG
endif # USE_SPDLOG

if ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
AM_CPPFLAGS += -DENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
endif # ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION

if BUILDING_NEXT_PROTOCOL
AM_CPPFLAGS += -DBUILDING_NEXT_PROTOCOL
endif

if ENABLE_CAP_0071
AM_CPPFLAGS += -DCAP_0071
endif

if ENABLE_TEST_FEATURE
AM_CPPFLAGS += -DTEST_FEATURE
endif
