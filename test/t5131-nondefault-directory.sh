#! /bin/sh -e
# tup - A file-based build system
# Copyright (C) 2026
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

. ./tup.sh

mkdir -p suite/run
cat > suite/Tupfile << HERE
: |> touch %o |> program
: |> ^n^ touch %o |> result.txt <tests>
HERE
cat > suite/run/Tupfile << HERE
: |> touch %o |> program
: |> ^n^ touch %o |> result.txt ../<tests>
HERE
update_partial suite
check_exist suite/program suite/run/program
check_not_exist suite/result.txt suite/run/result.txt
update_partial suite/run
check_not_exist suite/run/result.txt
update_partial .
check_not_exist suite/result.txt suite/run/result.txt

# An explicit output alongside a directory does not select its group peers.
update_partial suite suite/result.txt
check_exist suite/result.txt
check_not_exist suite/run/result.txt
update_partial 'suite/<tests>'
check_exist suite/run/result.txt

eotup
