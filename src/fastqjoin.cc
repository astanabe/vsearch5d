/*

  VSEARCH: a versatile open source tool for metagenomics

  Copyright (C) 2014-2024, Torbjorn Rognes, Frederic Mahe and Tomas Flouri
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
#include <cinttypes>  // macros PRIu64 and PRId64
#include <cstdint> // uint64_t
#include <cstdio>  // std::FILE, std::fprintf, std::fclose
#include <cstring>  // std::strlen, std::strcpy


/* static variables */

auto join_fileopenw(char * filename) -> std::FILE *
{
  std::FILE * file_ptr = nullptr;
  file_ptr = fopen_output(filename);
  if (not file_ptr)
    {
      fatal("Unable to open file for writing (%s)", filename);
    }
  return file_ptr;
}


auto fastq_join(struct Parameters const & parameters) -> void
{
  std::FILE * fp_fastqout = nullptr;
  std::FILE * fp_fastaout = nullptr;

  fastx_handle fastq_fwd = nullptr;
  fastx_handle fastq_rev = nullptr;

  uint64_t total = 0;

  /* check input and options */

  if (not parameters.opt_reverse)
    {
      fatal("No reverse reads file specified with --reverse");
    }

  if ((not parameters.opt_fastqout) and (not parameters.opt_fastaout))
    {
      fatal("No output files specified");
    }

  // bug fixing: if offset 64 then Q40 = 'h', not 'I'!

  auto const padlen = parameters.opt_join_padgap.length();

  if (parameters.opt_join_padgap.length() != parameters.opt_join_padgapq.length())
    {
      fatal("Strings given by --join_padgap and --join_padgapq differ in length");
    }

  /* open input files */

  fastq_fwd = fastq_open(parameters.opt_fastq_join);
  fastq_rev = fastq_open(parameters.opt_reverse);

  /* open output files */

  if (parameters.opt_fastqout)
    {
      fp_fastqout = join_fileopenw(parameters.opt_fastqout);
    }
  if (parameters.opt_fastaout)
    {
      fp_fastaout = join_fileopenw(parameters.opt_fastaout);
    }

  /* main */

  auto const filesize = fastq_get_size(fastq_fwd);
  progress_init("Joining reads", filesize);

  /* do it */

  total = 0;

  uint64_t alloc = 0;
  uint64_t len = 0;
  char * seq = nullptr;
  char * qual = nullptr;

  while (fastq_next(fastq_fwd, false, chrmap_no_change))
    {
      if (not fastq_next(fastq_rev, false, chrmap_no_change))
        {
          fatal("More forward reads than reverse reads");
        }

      auto const fwd_seq_length = fastq_get_sequence_length(fastq_fwd);
      auto const rev_seq_length = fastq_get_sequence_length(fastq_rev);

      /* allocate enough mem */

      auto const needed = fwd_seq_length + rev_seq_length + padlen + 1;
      if (alloc < needed)
        {
          seq = (char *) xrealloc(seq, needed);
          qual = (char *) xrealloc(qual, needed);
          alloc = needed;
        }

      /* join them */

      strcpy(seq, fastq_get_sequence(fastq_fwd));
      strcpy(qual, fastq_get_quality(fastq_fwd));
      len = fwd_seq_length;

      strcpy(seq + len, parameters.opt_join_padgap.data());
      strcpy(qual + len, parameters.opt_join_padgapq.data());
      len += padlen;

      /* reverse complement reverse read */

      char * rev_seq = fastq_get_sequence(fastq_rev);
      char * rev_qual = fastq_get_quality(fastq_rev);

      for (uint64_t i = 0; i < rev_seq_length; ++i)
        {
          auto const rev_pos = rev_seq_length - 1 - i;
          seq[len]  = chrmap_complement[(int) (rev_seq[rev_pos])];
          qual[len] = rev_qual[rev_pos];
          ++len;
        }
      seq[len] = 0;
      qual[len] = 0;

      /* write output */

      if (parameters.opt_fastqout)
        {
          fastq_print_general(fp_fastqout,
                              seq,
                              len,
                              fastq_get_header(fastq_fwd),
                              fastq_get_header_length(fastq_fwd),
                              qual,
                              0,
                              total + 1,
                              -1.0);
        }

      if (parameters.opt_fastaout)
        {
          fasta_print_general(fp_fastaout,
                              nullptr,
                              seq,
                              len,
                              fastq_get_header(fastq_fwd),
                              fastq_get_header_length(fastq_fwd),
                              0,
                              total + 1,
                              -1.0,
                              -1,
                              -1,
                              nullptr,
                              0);
        }

      ++total;
      progress_update(fastq_get_position(fastq_fwd));
    }

  progress_done();

  if (fastq_next(fastq_rev, false, chrmap_no_change))
    {
      fatal("More reverse reads than forward reads");
    }

  fprintf(stderr,
          "%" PRIu64 " pairs joined\n",
          total);

  /* clean up */

  if (parameters.opt_fastaout)
    {
      fclose(fp_fastaout);
    }
  if (parameters.opt_fastqout)
    {
      fclose(fp_fastqout);
    }

  fastq_close(fastq_rev);
  fastq_rev = nullptr;
  fastq_close(fastq_fwd);
  fastq_fwd = nullptr;

  if (seq)
    {
      xfree(seq);
    }
  if (qual)
    {
      xfree(qual);
    }
}
