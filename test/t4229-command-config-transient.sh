#! /bin/sh -e
# Text-only changes still force transient rules, but unexecuted reverts cancel.
. ./tup.sh
check_no_windows shell

mkdir compile tests
echo ': |> echo compiled > %o |> output' > compile/Tupfile
rules()
{
	cat > tests/Tupfile << HERE
: |> ^t^ echo $1 > %o |> intermediate
: intermediate |> cat %f > %o |> output
HERE
}
rules original
update
echo original | diff - tests/output
check_not_exist tests/intermediate

rules temporary
update_partial compile
rules original
# Missing transient outputs can retain ordinary modify-list entries even in a
# null build; only assert that restoration does not execute any commands.
update_partial > .tup/revert-output
grep 'No commands to execute' .tup/revert-output
update_null
check_not_exist tests/intermediate

# A real text mismatch must not be deferred just because the output is transient.
rules other
update_partial compile
update
echo other | diff - tests/output
check_not_exist tests/intermediate
update_null

# Failure is independent of the signature and survives text restoration.
cat > tests/Tupfile << 'HERE'
: |> ^t^ echo failed > %o && false |> intermediate
: intermediate |> cat %f > %o |> output
HERE
update_fail
rules other
update
echo other | diff - tests/output
check_not_exist tests/intermediate
update_null

eotup
