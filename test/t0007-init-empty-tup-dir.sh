#! /bin/sh -e
# tup - A file-based build system
#
# Copyright (C) 2026
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.

# Missing databases must not make an empty .tup directory a project root.
. ./tup.sh

tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/tup-t0007-XXXXXX")
cleanup()
{
	cd "$tupcurdir"
	rm -rf "$tmpdir"
}
trap cleanup EXIT INT TERM
cd "$tmpdir"

# Explicit initialization reuses the directory and preserves its options.
mkdir -p explicit/.tup
printf '[display]\ncolor = never\n' > explicit/.tup/options
cp explicit/.tup/options options.saved
tup init explicit
check_exist explicit/.tup/db
cmp options.saved explicit/.tup/options

# A Tupfile.ini also permits automatic initialization of an empty .tup.
mkdir -p automatic/.tup automatic/sub
touch automatic/Tupfile.ini
cd automatic/sub
tup parse
check_exist ../.tup/db
cd "$tmpdir"

# An empty child .tup must not hide a real ancestor database, even with ini.
mkdir -p explicit/child/.tup
touch explicit/child/Tupfile.ini
cd explicit/child
tup parse
check_not_exist .tup/db
if tup init; then
	echo 'Error: Expected initialization beneath an ancestor database to fail' >&2
	exit 1
fi
cd "$tmpdir"

# The requested directory's ancestors also count when invoked elsewhere.
if tup init explicit/child; then
	echo 'Error: Expected initialization of a target beneath an ancestor database to fail' >&2
	exit 1
fi
check_not_exist explicit/child/.tup/db

# Existing corrupt databases are neither ignored nor overwritten.
mkdir -p corrupt/.tup
touch corrupt/Tupfile.ini
printf 'not a sqlite database\n' > corrupt/.tup/db
cp corrupt/.tup/db corrupt.saved
cd corrupt
if tup parse; then
	echo 'Error: Expected parsing with a corrupt database to fail' >&2
	exit 1
fi
if tup init --force; then
	echo 'Error: Expected forced initialization with an existing database to fail' >&2
	exit 1
fi
cmp ../corrupt.saved .tup/db

eotup
