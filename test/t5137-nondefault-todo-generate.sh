#! /bin/sh -e
# tup - A file-based build system
# Copyright (C) 2026
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

. ./tup.sh

echo first > input.txt
cat > Tupfile << HERE
: input.txt |> cp %f %o |> program.txt
: program.txt |> ^n RUN-TEST^ cp %f %o |> result.txt <tests>
HERE
tup parse

# todo uses the same selection policy without executing the commands.
tup todo > .tup/default-todo
if grep 'RUN-TEST' .tup/default-todo; then
	echo 'Error: Default todo selected the test-run rule' >&2
	exit 1
fi
tup todo '<tests>' > .tup/test-todo
grep 'RUN-TEST' .tup/test-todo
check_not_exist program.txt result.txt

# Generated scripts must use the same default/directory/explicit selection.
generate .tup/default-$generate_script_name
if grep 'result.txt' .tup/default-$generate_script_name; then
	echo 'Error: Default generated script selected the test-run rule' >&2
	exit 1
fi
grep 'program.txt' .tup/default-$generate_script_name
generate .tup/directory-$generate_script_name .
if grep 'result.txt' .tup/directory-$generate_script_name; then
	echo 'Error: Directory generated script selected the test-run rule' >&2
	exit 1
fi
generate .tup/tests-$generate_script_name '<tests>'
grep 'result.txt' .tup/tests-$generate_script_name
grep 'program.txt' .tup/tests-$generate_script_name

eotup
