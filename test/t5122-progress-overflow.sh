#! /bin/sh -e
# tup - A file-based build system
#
# Copyright (C) 2026  Mike Shal <marfey@gmail.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

# Make sure large stored job times cannot overflow the progress percentage and
# make the progress bar exceed display.width.
. ./tup.sh
check_no_windows sqlite3 executable

cat > .tup/options << HERE
[display]
color = never
progress = 1
width = 20

[updater]
num_jobs = 1
HERE

cat > Tupfile << HERE
: |> touch %o |> out1
: |> touch %o |> out2
: |> touch %o |> out3
HERE
update

rm -f out1 out2 out3
sqlite3 .tup/db << HERE
update node set mtime = 30000000 where name in ('touch out1', 'touch out2', 'touch out3');
HERE

tup upd > .tup/progress-output
tr '\r' '\n' < .tup/progress-output | grep '^ \[' > .tup/progress-lines

if [ ! -s .tup/progress-lines ]; then
	echo "*** Expected progress output" 1>&2
	cat .tup/progress-output 1>&2
	exit 1
fi

while IFS= read -r line; do
	len=`printf "%s" "$line" | wc -c | sed 's/ //g'`
	if [ $len -gt 20 ]; then
		echo "*** Progress line exceeded display.width: $len" 1>&2
		printf "%s\n" "$line" 1>&2
		exit 1
	fi
done < .tup/progress-lines

eotup
