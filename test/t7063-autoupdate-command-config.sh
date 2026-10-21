#! /bin/sh -e
# Starting an autoupdate monitor must also pick up pending command-text changes.
. ./tup.sh
check_monitor_supported
check_no_windows shell
set_autoupdate

mkdir compile tests
echo ': |> echo compiled > %o |> output' > compile/Tupfile
echo ': |> echo original > %o |> output' > tests/Tupfile
update
echo ': |> echo pending > %o |> output' > tests/Tupfile
update_partial compile

monitor
tup flush
echo pending | diff - tests/output
stop_monitor

eotup
