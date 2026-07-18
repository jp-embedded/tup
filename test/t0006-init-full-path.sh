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

. ./tup.sh
# Windows/cygwin seems to get confused between /tmp/dir and c:/tmp/dir
check_no_windows full_path

# Running 'tup init' with an absolute path should still work.
tmpdir=`mktemp -d`
abspath="$tmpdir/foo/bar/baz"
tup init --force "$abspath"
if [ ! -f "$abspath/.tup/db" ]; then
	echo "$abspath/.tup/db not created!" 1>&2
	exit 1
fi
rm -rf "$tmpdir"

eotup
