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
cat > suite/Tupfile << HERE
: input.txt |> cp %f %o |> program.txt
: program.txt |> ^n^ cp %f %o |> result.txt ../<autotest>
HERE
update_partial build-one/suite
check_exist build-one/suite/program.txt
check_not_exist build-one/suite/result.txt build-two/suite/program.txt
update_partial 'build-one/<autotest>'
check_exist build-one/suite/result.txt
check_not_exist build-two/suite/result.txt

echo second > suite/input.txt
update_partial
echo second | diff - build-one/suite/program.txt
echo second | diff - build-two/suite/program.txt
echo first | diff - build-one/suite/result.txt
check_not_exist build-two/suite/result.txt
update_partial 'build-two/<autotest>'
echo second | diff - build-two/suite/result.txt
echo first | diff - build-one/suite/result.txt

eotup
