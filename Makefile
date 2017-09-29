# contrib/pg_stat_statements/Makefile

MODULE_big = vgram
OBJS = vgram.o vgram_gin.o vgram_like.o

EXTENSION = vgram
DATA = vgram--1.0.sql

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = ../vgram
top_builddir = ../postgresql
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
