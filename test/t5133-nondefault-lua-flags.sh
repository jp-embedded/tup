#! /bin/sh -e
# tup - A file-based build system
# Copyright (C) 2026
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

. ./tup.sh

echo first > input.txt
cat > Tupfile.lua << HERE
tup.rule({'input.txt'}, '^no COPY^ cat %f > %1o; cp %1o %2o', {'one.txt', 'two.txt'})
HERE
update
check_not_exist one.txt two.txt
update two.txt
echo first | diff - one.txt
echo first | diff - two.txt
update_null 'Selecting one output must update the whole rule only once.'

# Removing n restores default selection using existing flag-change handling.
cat > Tupfile.lua << HERE
tup.rule({'input.txt'}, '^o COPY^ cat %f > %1o; cp %1o %2o', {'one.txt', 'two.txt'})
HERE
echo second > input.txt
update
echo second | diff - one.txt
echo second | diff - two.txt

# Adding n preserves old outputs until the newly pending rule is selected.
cat > Tupfile.lua << HERE
tup.rule({'input.txt'}, '^no COPY^ cat %f > %1o; cp %1o %2o', {'one.txt', 'two.txt'})
HERE
echo third > input.txt
update
echo second | diff - one.txt
update one.txt
echo third | diff - one.txt
echo third | diff - two.txt

eotup
