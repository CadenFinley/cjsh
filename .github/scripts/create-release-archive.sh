#!/usr/bin/env sh

# create-release-archive.sh
#
# This file is part of cjsh, CJ's Shell
#
# MIT License
#
# Copyright (c) 2026 Caden Finley
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

set -eu

if [ "$#" -lt 3 ] || [ "$#" -gt 4 ]; then
    echo "usage: $0 <cjsh-binary> <version> <target> [output-directory]" >&2
    exit 2
fi

binary=$1
version=$2
target=$3
output_directory=${4:-dist}

if [ ! -x "$binary" ]; then
    echo "cjsh binary is missing or is not executable: $binary" >&2
    exit 1
fi
if ! printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$'; then
    echo "version must use the stable semantic version format X.Y.Z: $version" >&2
    exit 1
fi
if ! printf '%s\n' "$target" | grep -Eq '^[a-z0-9][a-z0-9_-]*$'; then
    echo "target contains unsupported characters: $target" >&2
    exit 1
fi

binary_tag=$("$binary" -c 'version --tag')
if [ "$binary_tag" != "v$version" ]; then
    echo "binary version $binary_tag does not match requested archive version v$version" >&2
    exit 1
fi

mkdir -p "$output_directory"
output_directory=$(cd "$output_directory" && pwd)
package_name="cjsh-v${version}-${target}"
archive_path="$output_directory/$package_name.tar.gz"
stage_root=$(mktemp -d "${TMPDIR:-/tmp}/cjsh-release.XXXXXX")
trap 'rm -rf "$stage_root"' EXIT HUP INT TERM

package_directory="$stage_root/$package_name"
mkdir -p "$package_directory"
cp "$binary" "$package_directory/cjsh"
chmod 0755 "$package_directory/cjsh"
cp LICENSE THIRD_PARTY_NOTICES README.md "$package_directory/"

tar -czf "$archive_path" -C "$stage_root" "$package_name"
printf '%s\n' "$archive_path"
