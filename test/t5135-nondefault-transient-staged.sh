#! /bin/sh -e
# tup - A file-based build system
# Copyright (C) 2026
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

. ./tup.sh

cat > Tupfile << HERE
: |> ^tn^ sh run.sh |> tmp1.txt tmp2.txt
: tmp1.txt |> ^n^ cp %f %o |> final1.txt
: tmp2.txt |> ^n^ cp %f %o |> final2.txt
HERE
cat > run.sh << HERE
echo first > tmp1.txt
echo second > tmp2.txt
HERE
update_partial
check_not_exist tmp1.txt tmp2.txt final1.txt final2.txt

# The existing partial-update lifetime applies to explicitly selected tn.
update_partial tmp1.txt
check_exist tmp1.txt tmp2.txt
update_partial
check_exist tmp1.txt tmp2.txt
check_not_exist final1.txt final2.txt

update_partial final1.txt
check_not_exist tmp1.txt
check_exist tmp2.txt
echo first | diff - final1.txt
update_partial
check_exist tmp2.txt
check_not_exist final2.txt

update_partial final2.txt
check_not_exist tmp1.txt tmp2.txt
echo second | diff - final2.txt

# Request a missing sibling while the other output is still staged.
update_partial tmp2.txt
check_exist tmp1.txt tmp2.txt
update_partial final1.txt
check_not_exist tmp1.txt
check_exist tmp2.txt
update_partial tmp1.txt
check_exist tmp1.txt tmp2.txt
update_partial final1.txt
check_not_exist tmp1.txt
check_exist tmp2.txt
update_partial final2.txt
check_not_exist tmp1.txt tmp2.txt

eotup
