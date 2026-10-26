#! /bin/sh -e
# Upgrading an existing database must establish safe per-command snapshots.
. ./tup.sh
check_no_windows sqlite3 executable

cat > Tupfile << 'HERE'
export FOO
: |> echo "$FOO" > %o |> output
HERE
export FOO=original
update

# Reproduce the version-19 schema, which has no successful-command history.
sqlite3 .tup/db << 'HERE'
drop trigger delete_command_config;
drop table command_config;
update config set rval=19 where lval='db_version';
HERE
update > .tup/upgrade-output
grep 'Updating tup database from version 19 to 21' .tup/upgrade-output
grep 'echo.*output' .tup/upgrade-output
check_exist .tup/db_backup_19
sqlite3 .tup/db "select count(*) from command_config where length(hash)=32 and pending=0" | grep '^1$'
update_null

# Deleting a command must delete its snapshot, even if its node ID is reused.
echo '' > Tupfile
update
sqlite3 .tup/db 'select count(*) from command_config' | grep '^0$'

eotup
