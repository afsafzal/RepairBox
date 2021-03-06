#!/bin/bash

# Retrieve and store the provided command-line arguments.
executable=$1
test_id=$2

# Find the directory that this test script belongs to.
here_dir=$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )
test_dir="$here_dir/test"

# Check if this test script is being used to compute coverage information.
[[ $(dirname $executable) = "coverage" ]] && coverage=0 || coverage=1
[[ $coverage = 0 ]] && timeout=10 || timeout=1

# Treats the test case as a positive test case.
exec_pos()
{
  timeout $timeout $executable < $test_dir/$test_id.in |&
    timeout 1 diff $test_dir/$test_id.out -
  return $?
}

# Treats the test case as a negative test case.
exec_neg()
{
  timeout $timeout $executable < $test_dir/$test_id.in &> /dev/null
  return $?
}

# Perform any necessary preparations before running the test case.
ulimit -c 8

# Execute the appropriate test case.
case $test_id in
  p1) exec_pos && exit 0;;
  p2) exec_pos && exit 0;;
  p3) exec_pos && exit 0;;
  p4) exec_pos && exit 0;;
  p5) exec_pos && exit 0;;
  n1) exec_neg && exit 0;;
esac 
rm -rf core*
exit 1
