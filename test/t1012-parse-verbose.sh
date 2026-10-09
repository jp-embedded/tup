#! /bin/sh -e
# tup - A file-based build system
#
# Copyright (C) 2026
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.

# Parser verbosity is independent of progress bars and build-job output.
. ./tup.sh

cat > .tup/options << HERE
[display]
color = never
width = 80
progress = 0
job_numbers = 1
job_time = 0
quiet = 0
HERE
mkdir parse-directory
echo ': |> echo built > %o |> result.txt' > parse-directory/Tupfile

# The default retains the successful per-directory result lines.
tup parse > .tup/default-output
grep 'parse-directory' .tup/default-output
check_not_exist parse-directory/result.txt

# Suppression must not prevent parsing or building, even without a bar.
echo 'parse_verbose = false' >> .tup/options
touch parse-directory/Tupfile
tup parse > .tup/quiet-output
if grep 'parse-directory' .tup/quiet-output; then
	echo 'Error: Non-verbose parsing printed a directory result' >&2
	exit 1
fi
touch parse-directory/Tupfile
tup > .tup/build-output
if grep 'parse-directory$' .tup/build-output; then
	echo 'Error: Non-verbose build printed a parsing result' >&2
	exit 1
fi
grep 'echo built' .tup/build-output
check_exist parse-directory/result.txt

# With progress enabled, completion is still reported without result lines.
echo 'progress = 1' >> .tup/options
touch parse-directory/Tupfile
tup parse > .tup/progress-output
grep '100%' .tup/progress-output
if grep 'parse-directory' .tup/progress-output; then
	echo 'Error: Non-verbose parsing with a bar printed a directory result' >&2
	exit 1
fi

# Explicit verbosity restores directory output, independently of the bar.
echo 'parse_verbose = true' >> .tup/options
touch parse-directory/Tupfile
tup parse > .tup/verbose-output
grep 'parse-directory' .tup/verbose-output
grep '100%' .tup/verbose-output

# Script generation does not use the updater's progress initialization.
# Its fresh in-memory database requires declared outputs not to exist yet.
rm parse-directory/result.txt
tup generate .tup/verbose-script.sh > .tup/generate-verbose-output
grep 'parse-directory' .tup/generate-verbose-output
echo 'parse_verbose = false' >> .tup/options
tup generate .tup/quiet-script.sh > .tup/generate-quiet-output
if grep 'parse-directory' .tup/generate-quiet-output; then
	echo 'Error: Non-verbose script generation printed a parsing result' >&2
	exit 1
fi

# Explicit Tupfile output is not hidden by the verbosity setting.
mkdir parser-message
echo 'print("parser-output-visible")' > parser-message/Tupfile.lua
tup parse > .tup/message-output
grep 'parser-output-visible' .tup/message-output

# A parser error and its directory context must remain visible.
echo 'this is not valid tup syntax' > parse-directory/Tupfile
if tup parse > .tup/error-output 2>&1; then
	echo 'Error: Expected invalid Tupfile to fail parsing' >&2
	exit 1
fi
grep 'parse-directory' .tup/error-output
grep 'Error\|error' .tup/error-output

eotup
