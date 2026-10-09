#! /bin/sh -e
# tup - A file-based build system
# Copyright (C) 2026
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

. ./tup.sh

echo first > input.txt
cat > Tupfile << HERE
: input.txt |> ^tn^ cp %f %o |> temporary.txt
: temporary.txt |> ^n^ cp %f %o |> result.txt <tests>
: input.txt |> ^nt^ cp %f %o |> independent.txt
HERE
update
check_not_exist temporary.txt result.txt independent.txt
update .
check_not_exist temporary.txt result.txt independent.txt
update '<tests>'
echo first | diff - result.txt
check_not_exist temporary.txt independent.txt

echo second > input.txt
update
echo first | diff - result.txt
check_not_exist temporary.txt independent.txt
update result.txt
echo second | diff - result.txt
check_not_exist temporary.txt independent.txt

# Adding an ordinary consumer pulls tn into default selection.
echo ': temporary.txt |> cp %f %o |> ordinary.txt' >> Tupfile
update
echo second | diff - ordinary.txt
check_not_exist temporary.txt independent.txt

# Explicit selection retains the pre-existing t cleanup behavior.
update independent.txt
check_not_exist independent.txt

eotup
