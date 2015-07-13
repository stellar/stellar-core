# Hand-written file with variables common to all makefiles

CPPFLAGS = -DASIO_SEPARATE_COMPILATION=1 -DSQLITE_OMIT_LOAD_EXTENSION=1
CPPFLAGS += -I"$(top_srcdir)" -I"$(top_srcdir)/src" -I"$(top_builddir)/src"
CPPFLAGS += $(libsodium_CFLAGS) $(xdrpp_CFLAGS) $(libmedida_CFLAGS)	\
	$(soci_CFLAGS)
CPPFLAGS += -I"$(top_srcdir)/lib"			\
	-I"$(top_srcdir)/lib/autocheck/include"		\
	-I"$(top_srcdir)/lib/cereal/include"		\
	-I"$(top_srcdir)/lib/asio/include"		\
	-I"$(top_srcdir)/lib/util"			\
	-I"$(top_srcdir)/lib/soci/src/core"		\
	-I"$(top_srcdir)/lib/soci/src/backends/sqlite3"

if USE_POSTGRES
CPPFLAGS += -DUSE_POSTGRES=1 $(libpq_CFLAGS)
endif # USE_POSTGRES
