#!/bin/bash

set -eux

if [ $COMPILER = "clang" ]; then
	export CC=clang-$LLVM_VER
else
	export CC=gcc
fi

if [ $CHECK_TYPE = "valgrind" ]; then
	sed -i.bak "s/\/\* #define USE_VALGRIND \*\//#define USE_VALGRIND/g" postgresql/src/include/pg_config_manual.h
fi

# configure & build
if [ $CHECK_TYPE = "normal" ]; then
	CONFIG_ARGS="--disable-debug --disable-cassert --enable-tap-tests --with-icu --prefix=$GITHUB_WORKSPACE/pgsql"
else
	CONFIG_ARGS="--enable-debug --enable-cassert --enable-tap-tests --with-icu --prefix=$GITHUB_WORKSPACE/pgsql"
fi

cd postgresql
./configure $CONFIG_ARGS
make -sj `nproc`
make -sj `nproc` install
cd ..

if [ $CHECK_TYPE = "static" ] && [ $COMPILER = "clang" ]; then
	sed -i.bak "s/ -Werror=unguarded-availability-new//g" pgsql/lib/pgxs/src/Makefile.global
fi

export PATH="$GITHUB_WORKSPACE/pgsql/bin:$PATH"

cd vgram
if [ $CHECK_TYPE = "sanitize" ]; then
	make -j `nproc` USE_PGXS=1 CFLAGS_SL="$(pg_config --cflags_sl) -Werror -fsanitize=alignment -fsanitize=address -fsanitize=undefined -fno-sanitize-recover=all -fno-sanitize=nonnull-attribute -fstack-protector" LDFLAGS_SL="-lubsan -fsanitize=address -fsanitize=undefined -lasan"
elif [ $CHECK_TYPE != "static" ]; then
	make -j `nproc` USE_PGXS=1 CFLAGS_SL="$(pg_config --cflags_sl) -Werror -coverage -fprofile-update=atomic"
fi
if [ $CHECK_TYPE != "static" ]; then
	make -j `nproc` USE_PGXS=1 install
fi
cd ..
