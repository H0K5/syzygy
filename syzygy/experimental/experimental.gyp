# Copyright 2012 Google Inc. All Rights Reserved.
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

{
  'variables': {
    'chromium_code': 1,
  },
  'targets': [
    {
      'target_name': 'experimental',
      'type': 'none',
      'dependencies': [
        '<(src)/syzygy/experimental/code_tally/code_tally.gyp:*',
        '<(src)/syzygy/experimental/compare/compare.gyp:*',
        '<(src)/syzygy/experimental/heap_enumerate/heap_enumerate.gyp:*',
        '<(src)/syzygy/experimental/pdb_dumper/pdb_dumper.gyp:*',
        '<(src)/syzygy/experimental/pdb_writer/pdb_writer.gyp:*',
        '<(src)/syzygy/experimental/protect/protect.gyp:*',
        '<(src)/syzygy/experimental/timed_decomposer/timed_decomposer.gyp:*',
      ],
    },
  ]
}
