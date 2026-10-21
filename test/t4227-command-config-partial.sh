#! /bin/sh -e
# Compare command text and environment with each command's successful execution.
. ./tup.sh
check_no_windows shell

mkdir compile tests follow
echo ': |> echo compiled > %o |> output' > compile/Tupfile
cat > tests/run.sh << 'HERE'
printf '%s:%s\n' "$1" "${FOO-unset}"
test "$1" != fail
HERE
cat > follow/Tupfile << 'HERE'
: ../tests/alpha |> cat %f > %o |> output
HERE
rules()
{
	cat > tests/Tupfile << HERE
export FOO
: |> sh run.sh $1 > %o |> alpha
: |> sh run.sh $1 > %o |> beta
HERE
}
rules original
export FOO=original
update

# An unexecuted command edit can be reverted without rebuilding its consumers.
rules temporary
update_partial compile
rules original
update > .tup/revert-output
grep 'No commands to execute' .tup/revert-output
update_null

# Parsing alone must not overwrite the successful signature either.
rules temporary
parse
rules original
update > .tup/revert-output
grep 'No commands to execute' .tup/revert-output
update_null

# Each command has its own combined signature, not a per-rule/per-invocation one.
rules other
export FOO=other
update_partial tests/alpha
echo other:other | diff - tests/alpha
echo original:original | diff - tests/beta
export FOO=third
update_partial compile
rules original
export FOO=original
update_partial tests/beta > .tup/partial-output
grep 'No commands to execute' .tup/partial-output
update
echo original:original | diff - tests/alpha
echo original:original | diff - follow/output
update_null

# Returning only the command OR only the environment is not a signature match.
rules other
update_partial compile
export FOO=other
rules original
update_partial tests/alpha
echo original:other | diff - tests/alpha
update
update_null

# Pending command changes survive updates with no further text or env change.
rules pending
update_partial compile
if tup flags_exists 2> .tup/pending-flags; then
	echo 'Expected pending configuration work' >&2
	exit 1
fi
grep configuration_list .tup/pending-flags
tup graph > .tup/pending-graph
grep 'sh run.sh pending' .tup/pending-graph
tup todo > .tup/pending-todo
grep 'sh run.sh pending' .tup/pending-todo
update_partial compile
update
echo pending:other | diff - tests/alpha
echo pending:other | diff - tests/beta
update_null

# Failed execution may damage outputs, even if its command is then restored.
rules fail
update_fail tests/alpha
rules pending
update
echo pending:other | diff - tests/alpha
echo pending:other | diff - follow/output
update_null

# With no shared output, a text edit can replace rather than rename the node.
# A newly created command has no successful signature and must execute.
mkdir actions
echo ': |> echo action-original |>' > actions/Tupfile
update
echo ': |> echo action-temporary |>' > actions/Tupfile
update_partial compile
echo ': |> echo action-original |>' > actions/Tupfile
update > .tup/new-node-output
grep 'actions: echo action-original' .tup/new-node-output
update_null

eotup
