PLUGINS = plpgsql_coverage
OBJS    = plpgsql_coverage.o

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/plpgsql_coverage
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

override CFLAGS += -I$(top_srcdir)/src/pl/plpgsql/src

all:    $(addsuffix $(DLSUFFIX), $(SHAREDLIBS)) $(addsuffix $(DLSUFFIX), $(PLUGINS)) $(INSTALL_scripts)

install: all installdirs
	$(INSTALL_SHLIB) $(addsuffix $(DLSUFFIX), $(PLUGINS)) '$(DESTDIR)$(pkglibdir)/plugins/'

installdirs:
	$(MKDIR_P)$(mkinstalldirs) $(DESTDIR)$(pkglibdir)/plugins

uninstall:
	rm -f $(addprefix '$(DESTDIR)$(pkglibdir)/plugins'/, $(addsuffix $(DLSUFFIX), $(PLUGINS)))
