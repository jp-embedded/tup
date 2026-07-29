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
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

# Make sure tup doesn't crash by parsing too many *'s in a row. Extra *'s
# should just be ignored.

. ./tup.sh
cat > Tupfile << HERE
: foreach ************.c |> gcc -c %f -o %o |> %B.o
HERE
touch foo.c
update
tup_object_exist . "gcc -c foo.c -o foo.o"

eotup
