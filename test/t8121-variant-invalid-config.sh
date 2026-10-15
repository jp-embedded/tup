#! /bin/sh -e
# tup - A file-based build system
# Copyright (C) 2026
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

# Invalid config arguments must fail before any variants are created.
. ./tup.sh

expect_failure()
{
	pattern=$1
	shift
	if tup variant "$@" 2> "$tupcurdir/$tuptestdir/.tup/variant-error"; then
		echo 'Error: Expected variant creation to reject an invalid config' >&2
		exit 1
	fi
	grep -F "$pattern" "$tupcurdir/$tuptestdir/.tup/variant-error"
}

expect_failure "Unable to access variant config file 'missing.config'" missing.config
check_not_exist build-missing

mkdir directory.config
expect_failure "Variant config file 'directory.config' is not a regular file" directory.config
check_not_exist build-directory

mkdir configs
touch configs/valid.config
expect_failure "Unable to access variant config file 'configs/missing.config'" configs/valid.config configs/missing.config
check_not_exist build-valid build-missing

# Validation must use the caller's directory, not a same-named root file.
touch missing.config
cd configs
expect_failure "Unable to access variant config file 'missing.config'" valid.config missing.config
check_not_exist ../build-valid ../build-missing
cd ..

# Invalid input must not alter an existing empty variant directory either.
mkdir build-valid
expect_failure "Unable to access variant config file 'configs/missing.config'" configs/valid.config configs/missing.config
check_not_exist build-valid/tup.config build-missing
test -z "$(ls -A build-valid)"

eotup
