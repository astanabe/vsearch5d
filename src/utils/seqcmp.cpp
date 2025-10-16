/*

  VSEARCH5D: a modified version of VSEARCH

  Copyright (C) 2016-2025, Akifumi S. Tanabe

  Contact: Akifumi S. Tanabe
  https://github.com/astanabe/vsearch5d

  Original version of VSEARCH
  Copyright (C) 2014-2025, Torbjorn Rognes, Frederic Mahe and Tomas Flouri
  All rights reserved.

  This software is dual-licensed and available under a choice
  of one of two licenses, either under the terms of the GNU
  General Public License version 3 or the BSD 2-Clause License.


  GNU General Public License version 3

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.


  The BSD 2-Clause License

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

  1. Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  2. Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
  COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.

*/

#include "maps.hpp"
#include "span.hpp"
#include <cassert>
#include <cstdint>  // uint64_t


// refactoring: with std::mismatch()? having to check if == '\0' breaks the flow

// Find first position with a difference, if any. Return 0 for
// identical sequences, -1 if lhs is sorted first (lower
// alpha-sorting), +1 if rhs is sorted first.
auto seqcmp(char const * lhs, char const * rhs, uint64_t const length) -> int {
  auto const lhs_seq = Span<char>{lhs, length};
  auto const rhs_seq = Span<char>{rhs, length};
  for (auto lhs_it = lhs_seq.cbegin(), rhs_it = rhs_seq.cbegin();
       lhs_it < lhs_seq.cend() and rhs_it < rhs_seq.cend();
       ++lhs_it, ++rhs_it) {
    if ((*lhs_it == '\0') or (*rhs_it == '\0')) {
      break;
    }
    auto const lhs_nuc = map_4bit(*lhs_it);
    auto const rhs_nuc = map_4bit(*rhs_it);
    if (lhs_nuc < rhs_nuc) {
      return -1;
    }
    if (lhs_nuc > rhs_nuc) {
      return +1;
    }
  }

  return 0;
}


auto seqcmp(char const * lhs, char const * rhs, unsigned int length) -> int {
  return seqcmp(lhs, rhs, static_cast<uint64_t>(length));
}


auto seqcmp(char const * lhs, char const * rhs, int length) -> int {
  assert(length >= 0);
  return seqcmp(lhs, rhs, static_cast<uint64_t>(length));
}


auto seqcmp(char const * lhs, char const * rhs, int64_t length) -> int {
  assert(length >= 0);
  return seqcmp(lhs, rhs, static_cast<uint64_t>(length));
}
