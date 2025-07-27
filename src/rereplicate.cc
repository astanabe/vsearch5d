/*

  VSEARCH: a versatile open source tool for metagenomics

  Copyright (C) 2014-2025, Torbjorn Rognes, Frederic Mahe and Tomas Flouri
  All rights reserved.

  Contact: Torbjorn Rognes <torognes@ifi.uio.no>,
  Department of Informatics, University of Oslo,
  PO Box 1080 Blindern, NO-0316 Oslo, Norway

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

#include "vsearch.h"
#include "utils/check_output_filehandle.hpp"
#include "utils/fatal.hpp"
#include "utils/maps.hpp"
#include "utils/open_file.hpp"
#include <cinttypes> // macros PRIu64 and PRId64
#include <cstdio>  // std::FILE, std::fprintf
#include <cstdint>  // int64_t


// anonymous namespace: limit visibility and usage to this translation unit
namespace {
  
}  // end of anonymous namespace


auto rereplicate(struct Parameters const & parameters) -> void
{
  auto const output_handle = open_output_file(parameters.opt_output);
  check_mandatory_output_handle(parameters.opt_output, (not output_handle));
  auto * input_handle = fasta_open(parameters.opt_rereplicate);
  auto const filesize = static_cast<int64_t>(fasta_get_size(input_handle));

  progress_init("Rereplicating", filesize);

  int64_t n_amplicons = 0;
  int64_t missing_abundance = 0;
  int64_t n_reads = 0;
  auto const truncateatspace = not parameters.opt_notrunclabels;
  while (fasta_next(input_handle, truncateatspace, chrmap_no_change_vector.data()))
    {
      ++n_amplicons;
      auto abundance = fasta_get_abundance_and_presence(input_handle);
      if (abundance == 0)
        {
          ++missing_abundance;
          abundance = 1;
        }

      for (int64_t i = 0; i < abundance; ++i)
        {
          ++n_reads;
          fasta_print_general(output_handle.get(),
                              nullptr,
                              fasta_get_sequence(input_handle),
                              static_cast<int>(fasta_get_sequence_length(input_handle)),
                              fasta_get_header(input_handle),
                              static_cast<int>(fasta_get_header_length(input_handle)),
                              1,
                              static_cast<int>(n_reads),
                              -1.0,
                              -1, -1, nullptr, 0.0);
        }

      progress_update(fasta_get_position(input_handle));
    }
  progress_done();

  if (not parameters.opt_quiet)
    {
      if (missing_abundance != 0)
        {
          std::fprintf(stderr, "WARNING: Missing abundance information for some input sequences, assumed 1\n");
        }
      std::fprintf(stderr, "Rereplicated %" PRId64 " reads from %" PRId64 " amplicons\n", n_reads, n_amplicons);
    }

  if (parameters.opt_log != nullptr)
    {
      if (missing_abundance != 0)
        {
          std::fprintf(stderr, "WARNING: Missing abundance information for some input sequences, assumed 1\n");
        }
      std::fprintf(fp_log, "Rereplicated %" PRId64 " reads from %" PRId64 " amplicons\n", n_reads, n_amplicons);
    }

  fasta_close(input_handle);
}
