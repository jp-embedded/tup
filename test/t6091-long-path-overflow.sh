#! /bin/sh -e
# tup - A file-based build system
#
# Copyright (C) 2008-2026  Mike Shal <marfey@gmail.com>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

# Verify that a path component longer than PATH_MAX in a Tupfile
# produces a proper error message instead of a buffer overflow.

. ./tup.sh

# Generate a filename longer than PATH_MAX (4096 on Linux)
LONG_NAME=$(python3 -c "print('a' * 5000)")

cat > Tupfile << HERE
: $LONG_NAME |> echo test |> out
HERE

update_fail_msg "Path too long (exceeds.*characters)"

eotup
