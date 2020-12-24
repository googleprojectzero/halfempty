#!/bin/sh
# Copyright 2018 Google Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# flag: --zero-char=32
# flag: --stable

# create a temporary file
tempfile=`mktemp`
result=1

# cleanup on exit
trap 'rm -f ${tempfile}; exit ${result}' EXIT TERM ALRM

# save data to temp file
cat > ${tempfile}

# make up some complicated rules
test $(grep -c A ${tempfile}) -ge 8 || exit                     # more than 8 lines with 'A'
test $(grep -c ^puppy$ ${tempfile}) -eq 1 || exit               # must contain a puppy
test $(sort ${tempfile} | uniq -d | wc -l) -eq 0  || exit       # no repeats

# looks good
result=0
