#! /bin/sh -e
# Variant consumers must retain generated input changes across partial updates.
. ./tup.sh
check_no_windows shell

mkdir package
cat > package/Tupfile << 'HERE'
: ../vers.txt |> cat %f > %o |> output
HERE
echo ': |> ^o^ echo version-3 > %o |> vers.txt' > Tupfile
touch release.config
tup variant release.config
update
echo version-3 | diff - build-release/package/output

echo ': |> ^o^ echo version-4 > %o |> vers.txt' > Tupfile
update_partial build-release/vers.txt
echo version-4 | diff - build-release/vers.txt
echo version-3 | diff - build-release/package/output
update_partial build-release/package/output
echo version-4 | diff - build-release/package/output
update_null

eotup
