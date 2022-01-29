#! /bin/sh -e
# tup - A file-based build system
#
# Copyright (C) 2010-2022  Mike Shal <marfey@gmail.com>
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

# Make sure that if a command creates a file and exits with a failure code,
# the outputs are still checked.
. ./tup.sh
check_no_windows shell

cat > Tupfile << HERE
: |> echo hey > ok.txt; exit 1 |>
HERE
update_fail_msg "failed with return value 1"

cat > Tupfile << HERE
HERE
update
check_not_exist ok.txt

eotup
