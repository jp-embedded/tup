#! /bin/sh -e
# Starting an autoupdate monitor must pick up environment-only pending work.
. ./tup.sh
check_monitor_supported
check_no_windows shell
set_autoupdate

mkdir compile tests
cat > compile/Tupfile << 'HERE'
: |> echo compiled > %o |> output
HERE
cat > tests/Tupfile << 'HERE'
export FOO
: |> printf '%%s\n' "$FOO" > %o |> output
HERE
export FOO=original
update
export FOO=pending
update_partial compile

# The monitor must use the cached environment of that update, not its own.
unset FOO
monitor
tup flush
echo pending | diff - tests/output
stop_monitor

eotup
