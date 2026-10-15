#! /bin/sh -e
# tup - A file-based build system
# Copyright (C) 2026
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

# Follow valid config symlinks, but reject dangling ones before mutation.
. ./tup.sh
check_no_windows symlinks

mkdir configs
echo CONFIG_DEBUG=y > configs/actual.config
ln -s actual.config configs/linked.config
ln -s missing.config configs/dangling.config
if tup variant configs/linked.config configs/dangling.config 2> .tup/variant-error; then
	echo 'Error: Expected dangling config symlink to fail' >&2
	exit 1
fi
grep -F "Unable to access variant config file 'configs/dangling.config'" .tup/variant-error
check_not_exist build-linked build-dangling

cat > Tupfile << HERE
ifeq (@(DEBUG),y)
: |> touch %o |> enabled.txt
endif
HERE
cd configs
tup variant linked.config
cd ..
test -L build-linked/tup.config
test "$(readlink build-linked/tup.config)" = '../configs/linked.config'
cmp configs/actual.config build-linked/tup.config
update
check_exist build-linked/enabled.txt

eotup
