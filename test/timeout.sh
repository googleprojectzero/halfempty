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

# flag: --timeout=1

# In bash, trap with the null string literally uses SIG_IGN, which child
# processes inherit. To avoid that, just use ':'. This means all the other
# processes in our group will get the default action for SIGALRM, but we will
# handle it. This way I can verify that the sleep command below really got
# killed with SIGALRM.
trap ':' ALRM

# Always timeout
sleep 60

if test $? -eq 142; then
    exit 0  # We were woken up with SIGALRM
else
    exit 1  # timeout didnt happen?
fi
