# contrib/pg_stat_statements/Makefile

MODULE_big = vgram
OBJS = vgram.o vgram_gin.o vgram_like.o vgram_selfunc.o vgram_typanalyze.o

EXTENSION = vgram
DATA = vgram--1.0.sql
PGFILEDESC = "vgram -- variable-length gram extranction and indexing"

REGRESS = vgram

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/vgram
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

ifdef TEMP_INSTANCE
installcheck: regresscheck
	echo "All checks are successful!"

PG_REGRESS_ARGS=--no-locale --temp-instance=./tmp_check

regresscheck: | submake-regress submake-vgram temp-install
	$(with_temp_install) $(pg_regress_check) \
		--temp-config regression.conf \
		$(PG_REGRESS_ARGS) \
		$(REGRESS)
endif

ifdef VALGRIND
override with_temp_install += PGCTLTIMEOUT=3000 \
	valgrind --vgdb=yes --leak-check=no --gen-suppressions=all \
	--suppressions=valgrind.supp --time-stamp=yes \
	--log-file=pid-%p.log --trace-children=yes \
	--trace-children-skip=*/initdb
else
override with_temp_install += PGCTLTIMEOUT=900
endif

vgram.typedefs: $(OBJS)
	./typedefs_gen.py

pgindent: vgram.typedefs
	pgindent --typedefs=vgram.typedefs *.c *.h

.PHONY: submake-vgram submake-regress check installcheck \
	regresscheck pgindent
