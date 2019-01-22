# Hand-written file with variables common to all makefiles

AM_CPPFLAGS = -isystem "$(top_srcdir)" -I"$(top_srcdir)/src" -I"$(top_builddir)/src"
AM_CPPFLAGS += $(libsodium_CFLAGS) $(xdrpp_CFLAGS) $(libmedida_CFLAGS)	\
	$(soci_CFLAGS) $(sqlite3_CFLAGS) $(libasio_CFLAGS)
AM_CPPFLAGS += -isystem "$(top_srcdir)/lib"			\
	-isystem "$(top_srcdir)/lib/autocheck/include"		\
	-isystem "$(top_srcdir)/lib/cereal/include"		\
	-isystem "$(top_srcdir)/lib/util"			\
	-isystem "$(top_srcdir)/lib/soci/src/core"

if USE_POSTGRES
AM_CPPFLAGS += -DUSE_POSTGRES=1 $(libpq_CFLAGS)
endif # USE_POSTGRES
