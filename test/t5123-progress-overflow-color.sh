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
# make the colored progress bar exceed display.width.
. ./tup.sh
check_no_windows sqlite3 executable

cat > .tup/options << HERE
[display]
color = always
progress = 1
width = 20

[updater]
num_jobs = 1
HERE

cat > Tupfile << HERE
: |> touch %o |> out1
: out1 |> touch %o |> out2
: out2 |> touch %o |> out3
: out3 |> touch %o |> out4
: out4 |> touch %o |> out5
: out5 |> touch %o |> out6
: out6 |> touch %o |> out7
: out7 |> touch %o |> out8
: out8 |> touch %o |> out9
: out9 |> touch %o |> out10
: out10 |> touch %o |> out11
: out11 |> touch %o |> out12
HERE
update

rm -f out1 out2 out3 out4 out5 out6 out7 out8 out9 out10 out11 out12
sqlite3 .tup/db << HERE
update node set mtime = 21474837 where name = 'touch out1';
update node set mtime = 1 where name in ('touch out2', 'touch out3', 'touch out4', 'touch out5', 'touch out6', 'touch out7', 'touch out8', 'touch out9', 'touch out10', 'touch out11', 'touch out12');
HERE

tup upd > .tup/progress-output 2>&1
esc=`printf '\033'`
tr '\r' '\n' < .tup/progress-output | sed "s/${esc}\[[0-9;]*m//g" | grep -a '^ \[' > .tup/progress-lines

if [ ! -s .tup/progress-lines ]; then
	echo "*** Expected progress output" 1>&2
	cat .tup/progress-output 1>&2
	exit 1
fi

while IFS= read -r line; do
	len=`printf "%s" "$line" | wc -c | sed 's/ //g'`
	if printf "%s\n" "$line" | grep -a -- '-[0-9][0-9]*%' > /dev/null; then
		echo "*** Progress percent went negative" 1>&2
		printf "%s\n" "$line" 1>&2
		exit 1
	fi
	if [ $len -gt 20 ]; then
		echo "*** Progress line exceeded display.width: $len" 1>&2
		printf "%s\n" "$line" 1>&2
		exit 1
	fi
done < .tup/progress-lines

eotup
