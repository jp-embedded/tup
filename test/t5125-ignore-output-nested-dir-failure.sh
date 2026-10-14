#! /bin/sh -e
# tup - A file-based build system
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
check_no_windows process

cat > Tupfile << 'HERE'
: run.sh |> exec sh %f |> report.xml ^/diagnostics(/.*)?$
: report.xml |> cp %f %o |> passed
HERE
cat > run.sh << 'HERE'
mkdir -p diagnostics/scenario/test-case
echo diagnostic > diagnostics/scenario/test-case/failure.txt
echo '<testsuite failures="1"/>' > report.xml
exit 7
HERE

update_fail_msg 'failed with return value 7'
echo diagnostic > expected
diff expected diagnostics/scenario/test-case/failure.txt
grep 'failures="1"' report.xml
if [ -e passed ]; then
	echo 'Dependent command ran after a failed command.' >&2
	exit 1
fi
if grep 'Unable to rename temporary file' .tup/.tupoutput; then
	echo 'Failed to publish excluded diagnostic.' >&2
	exit 1
fi
tup scan

# Failure must remain pending, not turn into a successful incremental build.
update_fail_msg 'failed with return value 7'
diff expected diagnostics/scenario/test-case/failure.txt

# Exercise the same publication path when the command is terminated by a signal.
cat > run.sh << 'HERE'
mkdir -p diagnostics/interrupted/test-case
echo diagnostic > diagnostics/interrupted/test-case/failure.txt
echo '<testsuite errors="1"/>' > report.xml
kill -TERM $$
HERE
update_fail_msg 'killed by signal 15'
diff expected diagnostics/interrupted/test-case/failure.txt
tup scan

cat > run.sh << 'HERE'
mkdir -p diagnostics/success/test-case
echo diagnostic > diagnostics/success/test-case/result.txt
echo '<testsuite failures="0"/>' > report.xml
HERE
update
diff expected diagnostics/success/test-case/result.txt
diff report.xml passed
tup scan
update

eotup
