# Hand-written file with variables common to all makefiles

AM_CPPFLAGS = -DASIO_SEPARATE_COMPILATION=1 -DSQLITE_OMIT_LOAD_EXTENSION=1
AM_CPPFLAGS += -I"$(top_srcdir)" -I"$(top_srcdir)/src" -I"$(top_builddir)/src"
AM_CPPFLAGS += $(libsodium_CFLAGS) $(xdrpp_CFLAGS) $(libmedida_CFLAGS)	\
	$(soci_CFLAGS) $(sqlite3_CFLAGS)
AM_CPPFLAGS += -I"$(top_srcdir)/lib"			\
	-I"$(top_srcdir)/lib/autocheck/include"		\
	-I"$(top_srcdir)/lib/cereal/include"		\
	-I"$(top_srcdir)/lib/asio/include"		\
	-I"$(top_srcdir)/lib/util"			\
	-I"$(top_srcdir)/lib/soci/src/core"

if USE_POSTGRES
AM_CPPFLAGS += -DUSE_POSTGRES=1 $(libpq_CFLAGS)
endif # USE_POSTGRES
