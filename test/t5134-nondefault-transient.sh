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
: input.txt |> ^nt^ echo independent-ran; cp %f %o |> independent.txt
: input.txt |> ^tn^ echo independent-group-ran; cp %f %o |> grouped.txt <independent>
HERE
update_partial
check_not_exist temporary.txt result.txt independent.txt grouped.txt
update_partial .
check_not_exist temporary.txt result.txt independent.txt grouped.txt
update_partial '<tests>'
echo first | diff - result.txt
check_not_exist temporary.txt independent.txt

echo second > input.txt
update_partial
echo first | diff - result.txt
check_not_exist temporary.txt independent.txt
update_partial result.txt
echo second | diff - result.txt
check_not_exist temporary.txt independent.txt

# Adding an ordinary consumer pulls tn into default selection.
rm result.txt
echo ': temporary.txt |> cp %f %o |> ordinary.txt' >> Tupfile
update_partial
echo second | diff - ordinary.txt
# The skipped result consumer still needs the newly staged transient file.
check_exist temporary.txt
check_not_exist result.txt independent.txt
update_partial result.txt
echo second | diff - result.txt
check_not_exist temporary.txt independent.txt

# Explicit selection retains the pre-existing t cleanup behavior.
update_partial independent.txt
check_not_exist independent.txt
update_partial independent.txt > .tup/independent-output
grep '^independent-ran$' .tup/independent-output
check_not_exist independent.txt
update_partial '<independent>' > .tup/independent-group-output
grep '^independent-group-ran$' .tup/independent-group-output
check_not_exist grouped.txt
update_partial '<independent>' > .tup/independent-group-output
grep '^independent-group-ran$' .tup/independent-group-output
check_not_exist grouped.txt

eotup
