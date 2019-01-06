#!/bin/bash

file=$1
project=$(echo "$2" | cut -d = -f 2)

if [[ "$file" = "" ]] ; then
    echo "Usage:  $0 <file with smatch messages> -p=<project>"
    exit 1
fi

if [[ "$project" != "kernel" ]] ; then
    exit 0
fi

bin_dir=$(dirname $0)
remove=$(echo ${bin_dir}/../smatch_data/kernel.implicit_dependencies.remove)
tmp=$(mktemp /tmp/smatch.XXXX)

# echo "// list of syscalls and the fields they write/read to." > kernel.implicit_dependencies
# echo '// generated by `gen_implicit_dependencies.sh`' >> kernel.implicit_dependencies
grep -w read_list $file >> $tmp
grep -w write_list $file >> $tmp
# cat $tmp $remove $remove 2> /dev/null | sort | uniq -u >> kernel.implicit_dependencies
cat $tmp >> kernel.implicit_dependencies
rm $tmp
echo "Done.  List saved as 'kernel.implicit_dependencies"
