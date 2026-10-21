#! /bin/sh -e
# Configuration-only changes cooperate with output comparison and output groups.
. ./tup.sh
check_no_windows shell

mkdir compile tests
echo ': |> echo compiled > %o |> output' > compile/Tupfile
rules()
{
	cat > tests/Tupfile << HERE
: |> ^o^ $1 > %o |> output <items>
: <items> |> cat %<items> > %o |> joined
HERE
}
rules 'echo original'
update
echo original | diff - tests/joined

rules "printf 'original\\n'"
update_partial compile
rules 'echo original'
update > .tup/revert-output
grep 'No commands to execute' .tup/revert-output
update_null

# A changed command must execute even if output comparison then finds its
# outputs unchanged. Its successful signature must still be updated.
rules "printf 'original\\n'"
update > .tup/same-output
grep 'printf' .tup/same-output
echo original | diff - tests/output
echo original | diff - tests/joined
update_null

# Changed contents must propagate through the group to the consumer.
rules 'echo changed'
update_partial compile
update
echo changed | diff - tests/output
echo changed | diff - tests/joined
update_null

eotup
