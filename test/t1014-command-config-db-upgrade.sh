#! /bin/sh -e
# Environment-only snapshots must not be mistaken for command signatures.
. ./tup.sh
check_no_windows sqlite3 executable

cat > Tupfile << 'HERE'
: |> echo original > %o |> output
HERE
update

# Reproduce the version-20 schema with a pending 32-byte hash. Deliberately copy
# the new signature's bytes: the upgrade must invalidate even a matching digest.
sqlite3 .tup/db << 'HERE'
create table command_env (id integer primary key not null, hash blob, pending integer not null default 0);
create index command_env_pending on command_env(pending) where pending=1;
create trigger delete_command_env after delete on node begin delete from command_env where id=old.id; end;
insert into command_env select id, hash, 1 from command_config;
drop trigger delete_command_config;
drop table command_config;
update config set rval=20 where lval='db_version';
HERE
update > .tup/upgrade-output
grep 'Updating tup database from version 20 to 21' .tup/upgrade-output
grep 'echo original' .tup/upgrade-output
check_exist .tup/db_backup_20
sqlite3 .tup/db "select count(*) from command_config where length(hash)=32 and pending=0" | grep '^1$'
sqlite3 .tup/db "select count(*) from sqlite_master where name='command_env'" | grep '^0$'
update_null

# A later text-only edit must use the newly established successful snapshot.
echo ': |> echo temporary > %o |> output' > Tupfile
parse
echo ': |> echo original > %o |> output' > Tupfile
update > .tup/revert-output
grep 'No commands to execute' .tup/revert-output
update_null

eotup
