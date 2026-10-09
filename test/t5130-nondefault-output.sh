#! /bin/sh -e
# tup - A file-based build system
# Copyright (C) 2026
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

. ./tup.sh

echo first > input.txt
cat > Tupfile << HERE
: input.txt |> cp %f %o |> ordinary.txt
: input.txt |> ^n^ cp %f %o |> optional.txt
HERE
update_partial
check_exist ordinary.txt
check_not_exist optional.txt
update_partial
check_not_exist optional.txt

update_partial optional.txt
echo first | diff - optional.txt
update_null 'Explicitly selected clean non-default outputs must not rerun.'

echo second > input.txt
update_partial
echo second | diff - ordinary.txt
echo first | diff - optional.txt
update_partial optional.txt
echo second | diff - optional.txt

rm optional.txt
update_partial
check_not_exist optional.txt
update_partial optional.txt
echo second | diff - optional.txt

eotup
