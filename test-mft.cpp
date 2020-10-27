// Copyright (c) 2020 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "MftParser.hpp"
#include <algorithm>
#include <boost/timer/timer.hpp>
#include <iostream>
#include <numeric>

int main() {
  boost::timer::auto_cpu_timer t;
  fsdb::MftParser parser;
  parser.open(L"\\\\?\\c:");
  auto files = parser.read_all();
  parser.close();

  auto total_count = files.size();

  auto total_size = std::accumulate(
      files.begin(), files.end(), std::size_t(0), [](std::size_t v, auto&& f) {
        if(f.name == "$BadClus") {
          return v;
        }

        return v + f.size;
      });

  std::cout << "test-mft found " << total_count << " files totalling "
            << total_size / 1024 << " KiB." << std::endl;

  std::partial_sort(
      files.begin(), files.begin() + 24, files.begin() + files.size(),
      [](auto&& a, auto&& b) { return a.size > b.size; });

  std::transform(
      files.begin(), files.begin() + 24,
      std::ostream_iterator<std::string>(std::cout, "\n"),
      [](auto&& f) { return f.name + ", " + std::to_string(f.size); });
  return 0;
}