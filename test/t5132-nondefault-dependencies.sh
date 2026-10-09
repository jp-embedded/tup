#! /bin/sh -e
# tup - A file-based build system
# Copyright (C) 2026
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

. ./tup.sh

echo first > input.txt
cat > Tupfile << HERE
: input.txt |> ^n^ cp %f %o |> direct.txt
: direct.txt |> cp %f %o |> direct-final.txt
: input.txt |> ^n^ cp %f %o |> grouped.txt <inputs>
: <inputs> |> cat %<inputs> > %o |> grouped-final.txt
: input.txt |> ^n^ cp %f %o |> chain.txt <chain>
: <chain> |> ^n^ cat %<chain> > %o |> chain-final.txt <tests>
: input.txt |> ^n^ cp %f %o |> unrelated.txt
HERE
update_partial
check_exist direct.txt direct-final.txt grouped.txt grouped-final.txt
check_not_exist chain.txt chain-final.txt unrelated.txt
update_partial '<tests>'
check_exist chain.txt chain-final.txt
check_not_exist unrelated.txt

echo second > input.txt
update_partial
echo second | diff - direct-final.txt
echo second | diff - grouped-final.txt
echo first | diff - chain-final.txt
update_partial '<tests>'
echo second | diff - chain-final.txt
check_not_exist unrelated.txt

eotup
