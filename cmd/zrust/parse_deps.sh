#!/bin/bash

[[ $# == 1 ]] || exit 1

target="$1"

if [[ ! -f $target ]]; then
	FILES="$(find . -name "*.rs" | sed 's@^./@@')"
else
	FILES="$(sed 's/.*: //' $target | { read -a files; for file in "${files[@]}"; do [[ "$file" != *".git"* ]] && realpath --relative-to="$PWD" "$file"; done })"
fi

echo "$FILES"

echo "$FILES" | sed 's@/.*@@' | sort | uniq | while read dir; do
	if [[ -f "${dir}/Cargo.toml" ]]; then
		echo "${dir}/Cargo.toml"
	fi
	if [[ -f "${dir}/Cargo.lock" ]]; then
		echo "${dir}/Cargo.lock"
	fi
done
echo Cargo.toml
echo Cargo.lock
