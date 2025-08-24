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
#include "dbindex.h"
#include "maps.h"
#include "mask.h"
#include "udb.h"
#include "unique.h"
#include "utils/fatal.hpp"
#include <cassert>
#include <cstdint>  // uint64_t
#include <cstdio>  // std::FILE, std::fprintf, std::size_t, std::fclose
#include <vector>


// documentation: assuming opt_wordlength = 3 (6 bits)
// input       output
// 0b000000 -> 0b111111
// 0b111111 -> 0b000000
// 0b111100 -> 0b110000
// 0b110000 -> 0b111100
// 0b001100 -> 0b110011
// 0b000011 -> 0b001111
// 0b001111 -> 0b000011
// 0b100001 -> 0b101101
// 0b011110 -> 0b010010
// 0b101010 -> 0b010101
// 0b010101 -> 0b101010
auto rc_kmer(unsigned int kmer) -> unsigned int
{
  /* reverse complement a kmer where k = opt_wordlength */

  assert(opt_wordlength * 2 <= 32);
  auto fwd = kmer;
  auto rev = 0U;

  for (auto i = int64_t{0}; i < opt_wordlength; ++i)
    {
      // compute complement of the last two bits
      auto const complement_bits = (fwd & 3U) ^ 3U;
      fwd = fwd >> 2U;
      rev = rev << 2U;
      rev |= complement_bits;
    }

  return rev;
}


auto orient(struct Parameters const & parameters) -> void
{
  fastx_handle query_h = nullptr;
  // refactoring: use struct, like in subsample
  std::FILE * fp_fastaout = nullptr;
  std::FILE * fp_fastqout = nullptr;
  std::FILE * fp_tabbedout = nullptr;
  std::FILE * fp_notmatched = nullptr;

  int queries = 0;
  int qmatches = 0;
  int matches_fwd = 0;
  int matches_rev = 0;
  int notmatched = 0;

  /* check arguments */

  if (opt_db == nullptr)
    {
      fatal("Database not specified with --db");
    }

  if (not ((opt_fastaout != nullptr) or (opt_fastqout != nullptr) or (opt_notmatched != nullptr) or (opt_tabbedout != nullptr)))
    {
      fatal("Output file not specified with --fastaout, --fastqout, --notmatched or --tabbedout");
    }

  /* prepare reading of queries */

  query_h = fastx_open(parameters.opt_orient);

  /* open output files */

  if (opt_fastaout != nullptr)
    {
      fp_fastaout = fopen_output(opt_fastaout);
      if (fp_fastaout == nullptr)
        {
          fatal("Unable to open fasta output file for writing");
        }
    }

  if (opt_fastqout != nullptr)
    {
      if (not fastx_is_fastq(query_h))
        {
          fatal("Cannot write FASTQ output with FASTA input");
        }

      fp_fastqout = fopen_output(opt_fastqout);
      if (fp_fastqout == nullptr)
        {
          fatal("Unable to open fastq output file for writing");
        }
    }

  if (opt_notmatched != nullptr)
    {
      fp_notmatched = fopen_output(opt_notmatched);
      if (fp_notmatched == nullptr)
        {
          fatal("Unable to open notmatched output file for writing");
        }
    }

  if (opt_tabbedout != nullptr)
    {
      fp_tabbedout = fopen_output(opt_tabbedout);
      if (fp_tabbedout == nullptr)
        {
          fatal("Unable to open tabbedout output file for writing");
        }
    }

  /* check if it may be an UDB file */

  auto const is_udb = udb_detect_isudb(opt_db);

  if (is_udb)
    {
      udb_read(opt_db, true, true);
    }
  else
    {
      db_read(opt_db, 0);
    }

  if (not is_udb)
    {
      if (opt_dbmask == MASK_DUST)
        {
          dust_all();
        }
      else if ((opt_dbmask == MASK_SOFT) and (opt_hardmask != 0))
        {
          hardmask_all();
        }
    }

  if (not is_udb)
    {
      dbindex_prepare(1, opt_dbmask);
      dbindex_addallsequences(opt_dbmask);
    }

  uhandle_s * uh_fwd = unique_init();

  std::size_t alloc = 0;
  std::vector<char> qseq_rev;
  std::vector<char> query_qual_rev;

  progress_init("Orienting sequences", fasta_get_size(query_h));

  while (fastx_next(query_h,
                    (opt_notrunclabels == 0),
                    chrmap_no_change))
    {
      char const * query_head = fastx_get_header(query_h);
      int const query_head_len = fastx_get_header_length(query_h);
      char const * qseq_fwd = fastx_get_sequence(query_h);
      int const qseqlen = fastx_get_sequence_length(query_h);
      int const qsize = fastx_get_abundance(query_h);
      char * query_qual_fwd = fastx_get_quality(query_h);

      /* find kmers in query sequence */

      unsigned int kmer_count_fwd = 0;
      unsigned int * kmer_list_fwd = nullptr;

      unique_count(uh_fwd, opt_wordlength, qseqlen, qseq_fwd,
                   & kmer_count_fwd, & kmer_list_fwd, opt_qmask);

      /* count kmers matching on each strand */

      unsigned int count_fwd = 0;
      unsigned int count_rev = 0;
      constexpr auto hits_factor = 8U;

      for (unsigned int i = 0; i < kmer_count_fwd; i++)
        {
          unsigned int const kmer_fwd = kmer_list_fwd[i];
          unsigned int const kmer_rev = rc_kmer(kmer_fwd);

          unsigned int const hits_fwd = dbindex_getmatchcount(kmer_fwd);
          unsigned int const hits_rev = dbindex_getmatchcount(kmer_rev);

          /* require 8 times as many matches on one stand than the other */

          if (hits_fwd > hits_factor * hits_rev)
            {
              ++count_fwd;
            }
          else if (hits_rev > hits_factor * hits_fwd)
            {
              ++count_rev;
            }
        }

      /* get progress as amount of input file read */

      auto const progress = fasta_get_position(query_h);

      /* update stats */

      ++queries;

      auto strand = 2;  // refactoring: enum struct Strand : char {positive = '+', negative = '-', undetermined = '?'};
      auto const min_count = 1U;
      auto const min_factor = 4U;

      if ((count_fwd >= min_count) and (count_fwd >= min_factor * count_rev))
        {
          /* fwd */

          strand = 0;
          ++matches_fwd;
          ++qmatches;

          if (opt_fastaout != nullptr)
            {
              fasta_print_general(fp_fastaout,
                                  nullptr,
                                  qseq_fwd,
                                  qseqlen,
                                  query_head,
                                  query_head_len,
                                  qsize,
                                  qmatches,
                                  -1.0,
                                  -1,
                                  -1,
                                  nullptr,
                                  0.0);
            }

          if (opt_fastqout != nullptr)
            {
              fastq_print_general(fp_fastqout,
                                  qseq_fwd,
                                  qseqlen,
                                  query_head,
                                  query_head_len,
                                  query_qual_fwd,
                                  qsize,
                                  qmatches,
                                  -1.0);
            }
        }
      else if ((count_rev >= min_count) and (count_rev >= min_factor * count_fwd))
        {
          /* rev */

          strand = 1;
          ++matches_rev;
          ++qmatches;

          /* alloc more mem if necessary to keep reverse sequence and qual */
          assert(qseqlen > 0);
          static_assert(sizeof(std::size_t) >= sizeof(int), "size_t is too small");
          const std::size_t requirements = qseqlen + 1;
          // refactoring: unsigned int qseqlen
          if (requirements > alloc)
            {
              alloc = requirements;
              qseq_rev.resize(alloc);
              if (fastx_is_fastq(query_h))
                {
                  query_qual_rev.resize(alloc);
                }
            }

          /* get reverse complementary sequence */

          reverse_complement(qseq_rev.data(), qseq_fwd, qseqlen);

          if (opt_fastaout != nullptr)
            {
              fasta_print_general(fp_fastaout,
                                  nullptr,
                                  qseq_rev.data(),
                                  qseqlen,
                                  query_head,
                                  query_head_len,
                                  qsize,
                                  qmatches,
                                  -1.0,
                                  -1,
                                  -1,
                                  nullptr,
                                  0.0);
            }

          if (opt_fastqout != nullptr)
            {
              /* reverse quality scores */

              if (fastx_is_fastq(query_h))
                {
                  // copy query string in reverse order
                  for (int i = 0; i < qseqlen; i++)
                    {
                      query_qual_rev[i] = query_qual_fwd[qseqlen - 1 - i];
                    }
                  query_qual_rev[qseqlen] = '\0';
                }

              fastq_print_general(fp_fastqout,
                                  qseq_rev.data(),
                                  qseqlen,
                                  query_head,
                                  query_head_len,
                                  query_qual_rev.data(),
                                  qsize,
                                  qmatches,
                                  -1.0);
            }
        }
      else
        {
          /* undecided */

          strand = 2;
          ++notmatched;

          if (opt_notmatched != nullptr)
            {
              if (fastx_is_fastq(query_h))
                {
                  fastq_print_general(fp_notmatched,
                                      qseq_fwd,
                                      qseqlen,
                                      query_head,
                                      query_head_len,
                                      query_qual_fwd,
                                      qsize,
                                      notmatched,
                                      -1.0);
                }
              else
                {
                  fasta_print_general(fp_notmatched,
                                      nullptr,
                                      qseq_fwd,
                                      qseqlen,
                                      query_head,
                                      query_head_len,
                                      qsize,
                                      notmatched,
                                      -1.0,
                                      -1,
                                      -1,
                                      nullptr,
                                      0.0);
                }
            }
        }

      if (opt_tabbedout != nullptr)
        {
          fprintf(fp_tabbedout,
                  "%s\t%c\t%d\t%d\n",
                  query_head,
                  strand == 0 ? '+' : (strand == 1 ? '-' : '?'),
                  count_fwd,
                  count_rev);
        }

      /* show progress */

      progress_update(progress);
    }

  progress_done();

  /* clean up */

  unique_exit(uh_fwd);

  dbindex_free();
  db_free();

  if (opt_tabbedout != nullptr)
    {
      fclose(fp_tabbedout);
    }
  if (opt_notmatched != nullptr)
    {
      fclose(fp_notmatched);
    }
  if (opt_fastqout != nullptr)
    {
      fclose(fp_fastqout);
    }
  if (opt_fastaout != nullptr)
    {
      fclose(fp_fastaout);
    }

  fasta_close(query_h);

  if (not opt_quiet)
    {
      fprintf(stderr, "Forward oriented sequences: %d", matches_fwd);
      if (queries > 0)
        {
          fprintf(stderr, " (%.2f%%)", 100.0 * matches_fwd / queries);
        }
      fprintf(stderr, "\n");
      fprintf(stderr, "Reverse oriented sequences: %d", matches_rev);
      if (queries > 0)
        {
          fprintf(stderr, " (%.2f%%)", 100.0 * matches_rev / queries);
        }
      fprintf(stderr, "\n");
      fprintf(stderr, "All oriented sequences:     %d", qmatches);
      if (queries > 0)
        {
          fprintf(stderr, " (%.2f%%)", 100.0 * qmatches / queries);
        }
      fprintf(stderr, "\n");
      fprintf(stderr, "Not oriented sequences:     %d", notmatched);
      if (queries > 0)
        {
          fprintf(stderr, " (%.2f%%)", 100.0 * notmatched / queries);
        }
      fprintf(stderr, "\n");
      fprintf(stderr, "Total number of sequences:  %d\n", queries);
    }

  if (opt_log != nullptr)
    {
      fprintf(fp_log, "Forward oriented sequences: %d", matches_fwd);
      if (queries > 0)
        {
          fprintf(fp_log, " (%.2f%%)", 100.0 * matches_fwd / queries);
        }
      fprintf(fp_log, "\n");
      fprintf(fp_log, "Reverse oriented sequences: %d", matches_rev);
      if (queries > 0)
        {
          fprintf(fp_log, " (%.2f%%)", 100.0 * matches_rev / queries);
        }
      fprintf(fp_log, "\n");
      fprintf(fp_log, "All oriented sequences:     %d", qmatches);
      if (queries > 0)
        {
          fprintf(fp_log, " (%.2f%%)", 100.0 * qmatches / queries);
        }
      fprintf(fp_log, "\n");
      fprintf(fp_log, "Not oriented sequences:     %d", notmatched);
      if (queries > 0)
        {
          fprintf(fp_log, " (%.2f%%)", 100.0 * notmatched / queries);
        }
      fprintf(fp_log, "\n");
      fprintf(fp_log, "Total number of sequences:  %d\n", queries);
    }
}
