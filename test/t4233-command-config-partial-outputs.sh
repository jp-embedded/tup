#! /bin/sh -e
# Generated input changes must survive a configuration-only partial update.
. ./tup.sh
check_no_windows shell

mkdir package
cat > package/Tupfile << 'HERE'
: ../vers.txt |> cat %f > %o |> output
HERE
echo ': |> ^o^ echo version-3 > %o |> vers.txt' > Tupfile
update
echo version-3 | diff - package/output

# The producer's command changes, but its consumer's command does not.
echo ': |> ^o^ echo version-4 > %o |> vers.txt' > Tupfile
update_partial vers.txt
echo version-4 | diff - vers.txt
echo version-3 | diff - package/output

# Selecting the consumer later must pick up the generated input change.
update_partial package/output
echo version-4 | diff - package/output
update_null

# The deferred input change must also survive until a full update.
echo ': |> ^o^ echo version-5 > %o |> vers.txt' > Tupfile
update_partial vers.txt
echo version-4 | diff - package/output
update
echo version-5 | diff - package/output
update_null

# A changed command that produces identical output must not dirty its consumer.
cat > Tupfile << 'HERE'
: |> ^o^ printf 'version-5\n' > %o |> vers.txt
HERE
update_partial vers.txt
update_null

# An intermediate selected in the first update must not lose a later package.
mkdir library chainpackage
cat > library/Tupfile << 'HERE'
: ../vers.txt |> ^o^ cat %f > %o |> output
HERE
cat > chainpackage/Tupfile << 'HERE'
: ../vers.txt ../library/output |> cat %f > %o |> output
HERE
update
echo ': |> ^o^ echo version-6 > %o |> vers.txt' > Tupfile
update_partial library/output
echo version-6 | diff - library/output
printf 'version-5\nversion-5\n' | diff - chainpackage/output
update_partial chainpackage/output
printf 'version-6\nversion-6\n' | diff - chainpackage/output
update
update_null

# The same invalidation is needed when only the producer's environment changes.
mkdir environment envpackage
cat > environment/Tupfile << 'HERE'
export PARTIAL_VERSION
: |> ^o^ sh generate.sh > %o |> vers.txt
HERE
cat > environment/generate.sh << 'HERE'
printf '%s\n' "${PARTIAL_VERSION%%:*}"
HERE
cat > envpackage/Tupfile << 'HERE'
: ../environment/vers.txt |> cat %f > %o |> output
HERE
export PARTIAL_VERSION=version-3
update
echo version-3 | diff - envpackage/output
export PARTIAL_VERSION=version-4
update_partial environment/vers.txt
echo version-4 | diff - environment/vers.txt
echo version-3 | diff - envpackage/output
update_partial envpackage/output
echo version-4 | diff - envpackage/output
update_null

# Environment-only changes with identical output must still honor ^o.
export PARTIAL_VERSION=version-4:unchanged
update_partial environment/vers.txt
update_null

eotup
