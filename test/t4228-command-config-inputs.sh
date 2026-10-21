#! /bin/sh -e
# Restoring command text cannot cancel independent file, flag or graph changes.
. ./tup.sh
check_no_windows shell

mkdir compile tests source follow
echo ': |> echo compiled > %o |> output' > compile/Tupfile
echo ': input |> cat %f > %o |> output' > source/Tupfile
echo first > source/input
echo 'cat ../source/output' > tests/run.sh
cat > follow/Tupfile << 'HERE'
: ../tests/output |> cat %f > %o |> output
HERE
rules()
{
	echo ": ../source/output |> $1 sh run.sh $2 > %o |> output" > tests/Tupfile
}
rules '' original
update

# A changed generated predecessor must remain dirty through edit/revert.
rules '' temporary
echo second > source/input
update_partial compile
rules '' original
update
echo second | diff - tests/output
echo second | diff - follow/output
update_null

# So must a directly discovered input change.
rules '' temporary
echo 'echo third' > tests/run.sh
update_partial compile
rules '' original
update
echo third | diff - tests/output
echo third | diff - follow/output
update_null

# Flags are deliberately not reversible signature causes.
rules '^s^' temporary
update_partial compile
rules '' original
update > .tup/flag-output
grep 'sh run.sh original' .tup/flag-output
update_null

# Dependency changes continue through the existing conservative dirty handling.
echo ': run.sh ../source/output |> sh run.sh temporary > %o |> output' > tests/Tupfile
update_partial compile
rules '' original
update > .tup/input-output
grep 'sh run.sh original' .tup/input-output
update_null

# Adding and then removing an output must not be cancelled by text restoration.
echo ': ../source/output |> sh run.sh original > output; touch extra-output |> output extra-output' > tests/Tupfile
update_partial compile
rules '' original
update > .tup/output-output
grep 'sh run.sh original' .tup/output-output
update_null

# A missing output is independent too.
rules '' temporary
rm tests/output
update_partial compile
rules '' original
update
echo third | diff - tests/output
update_null

eotup
