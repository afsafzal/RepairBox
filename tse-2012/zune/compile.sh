#!/bin/bash
here_dir=$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )
TIME_LIMIT=5
exe=$1
exe_dir=$(dirname "$exe")

pushd "${exe_dir}"
timeout $TIME_LIMIT gcc zune.c -o zune && exit 0
exit 1
