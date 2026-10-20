#! /bin/sh -e
# An unrelated partial update must not forget a command's successful environment.
. ./tup.sh
check_no_windows shell

mkdir compile tests follow
cat > compile/Tupfile << 'HERE'
: |> echo compiled > %o |> output
HERE
cat > tests/Tupfile << 'HERE'
export FOO
: |> sh run.sh > %o |> alpha
: |> sh run.sh > %o |> beta
HERE
cat > tests/run.sh << 'HERE'
printf '%s\n' "${FOO-unset}"
HERE
cat > follow/Tupfile << 'HERE'
: ../tests/alpha |> cat %f > %o |> output
HERE

export FOO=original
update

# This is the local compile-without-arguments, then test-with-arguments workflow.
unset FOO
update_partial compile
export FOO=original
update_null

# The snapshot belongs to each command, not to the last invocation or variable.
export FOO=other
update_partial tests/alpha
echo other | diff - tests/alpha
echo original | diff - tests/beta
export FOO=third
update_partial compile
export FOO=original
update_partial tests/beta > .tup/partial-output
grep 'No commands to execute' .tup/partial-output
echo other | diff - tests/alpha
update
echo original | diff - tests/alpha
echo original | diff - follow/output
update_null

# An environment-only mismatch must persist across unrelated invocations even
# when the variable no longer changes between those invocations.
export FOO=pending
update_partial compile
if tup flags_exists 2> .tup/pending-flags; then
	echo 'Expected pending environment work' >&2
	exit 1
fi
grep environment_list .tup/pending-flags
tup graph > .tup/pending-graph
grep 'run.sh' .tup/pending-graph
update_partial compile
update
echo pending | diff - tests/alpha
echo pending | diff - tests/beta
echo pending | diff - follow/output
update_null

# Unset and set-to-empty are different environments.
unset FOO
update
echo unset | diff - tests/alpha
export FOO=
update
echo '' | diff - tests/alpha
unset FOO
update_partial compile
export FOO=
update_null

# --no-environ-check continues using the cached environment, not getenv().
export FOO=ignored
update --no-environ-check
echo '' | diff - tests/alpha
update
echo ignored | diff - tests/alpha

eotup
