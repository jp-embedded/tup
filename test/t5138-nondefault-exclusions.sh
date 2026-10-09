#! /bin/sh -e
# tup - A file-based build system
# Copyright (C) 2026
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.

. ./tup.sh

mkdir suite build-one build-two
touch build-one/tup.config build-two/tup.config
echo first > suite/input.txt
cat > suite/Tupfile << 'HERE'
: input.txt |> cp %f %o |> program.txt
: program.txt |> ^bn RUN^ mkdir -p diagnostics; cp %f %o; echo scratch > ignored.gcda; echo diagnostic > diagnostics/log |> result.txt | ^.gcda$ ^/diagnostics(/|$)
: result.txt |> ^n TAG^ cp %f %o |> marker.txt ../<tests>
HERE

update_partial
check_exist build-one/suite/program.txt build-two/suite/program.txt
check_not_exist build-one/suite/result.txt build-one/suite/marker.txt build-two/suite/result.txt
update_partial build-one/suite
check_not_exist build-one/suite/result.txt build-one/suite/marker.txt
update_partial 'build-one/<tests>'
echo first | diff - build-one/suite/result.txt
check_exist build-one/suite/marker.txt
check_not_exist build-two/suite/result.txt

echo second > suite/input.txt
update_partial
echo second | diff - build-one/suite/program.txt
echo first | diff - build-one/suite/result.txt
echo first | diff - build-one/suite/marker.txt
update_partial 'build-one/<tests>'
echo second | diff - build-one/suite/result.txt
echo second | diff - build-one/suite/marker.txt
update_partial build-one/suite/result.txt
eotup
