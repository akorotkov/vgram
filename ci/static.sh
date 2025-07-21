#!/bin/bash

set -eu
export PATH="$GITHUB_WORKSPACE/pgsql/bin:$PATH"

status=0

cd vgram
if [ "$COMPILER" = "clang" ]; then
	scan-build-$LLVM_VER --status-bugs \
		-disable-checker deadcode.DeadStores \
		make USE_PGXS=1 USE_ASSERT_CHECKING=1 || status=$?

elif [ "$COMPILER" = "gcc" ]; then
	cppcheck \
		--enable=warning,portability,performance \
		--suppress=redundantAssignment \
		--suppress=uselessAssignmentPtrArg \
		--suppress=incorrectStringBooleanError \
		--suppress=nullPointerRedundantCheck \
		--std=c89 --inline-suppr --verbose *.c *.h -I $GITHUB_WORKSPACE/pgsql/include 2> cppcheck.log

	if [ -s cppcheck.log ]; then
		cat cppcheck.log
		status=1 # error
	fi
fi
cd ..

exit $status
