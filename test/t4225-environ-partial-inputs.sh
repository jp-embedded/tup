#! /bin/sh -e
# Environment changes must never mask pending file/rule changes or failures.
. ./tup.sh
check_no_windows shell

mkdir compile tests follow source
cat > compile/Tupfile << 'HERE'
: |> echo compiled > %o |> output
HERE
cat > tests/Tupfile << 'HERE'
export FOO
: ../source/output |> sh run.sh > %o |> output
HERE
cat > tests/run.sh << 'HERE'
test "$FOO" != fail || exit 1
echo first
HERE
cat > source/Tupfile << 'HERE'
: input |> cat %f > %o |> output
HERE
echo input > source/input
cat > follow/Tupfile << 'HERE'
: ../tests/output |> cat %f > %o |> output
HERE
export FOO=original
update

# A real source change seen during the unrelated update must survive a later
# return to the successful environment, including its downstream consumers.
cat > tests/run.sh << 'HERE'
test "$FOO" != fail || exit 1
echo second
HERE
unset FOO
update_partial compile
export FOO=original
update
echo second | diff - tests/output
echo second | diff - follow/output
update_null

# A changed generated input is also an independent cause, even if the command
# was initially added to the graph because of its environment.
cat > tests/run.sh << 'HERE'
test "$FOO" != fail || exit 1
cat ../source/output
HERE
update
echo new-input > source/input
unset FOO
update_partial compile
export FOO=original
update
echo new-input | diff - tests/output
echo new-input | diff - follow/output
update_null

# Execution that fails must not update the successful snapshot or become clean
# when its environment reverts.
export FOO=fail
update_fail tests/output
export FOO=original
update
echo new-input | diff - tests/output
update_null

# A real command change must also survive the environment reverting.
cat > tests/Tupfile << 'HERE'
export FOO
: ../source/output |> sh run.sh | tr a-z A-Z > %o |> output
HERE
unset FOO
update_partial compile
export FOO=original
update
echo NEW-INPUT | diff - tests/output
echo NEW-INPUT | diff - follow/output
update_null

eotup
