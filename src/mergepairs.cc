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
#include "maps.h"
#include "utils/fatal.hpp"
#include "utils/xpthread.hpp"
#include <algorithm>  // std::min, std::max
#include <array>
#include <cassert>
#include <cinttypes>  // macros PRIu64 and PRId64
#include <cmath>  // std::pow, std::sqrt, std::round, std::log10, std::log2
#include <cstdint>  // int64_t, uint64_t
#include <cstdio>  // std::FILE, std::fprintf, std::fclose
#include <cstdlib>  // std::exit, EXIT_FAILURE
#include <cstring>  // std::strcpy, std::strlen
#include <pthread.h>
#include <vector>


/* chunk constants */

constexpr auto chunk_size = 500; /* read pairs per chunk */
constexpr auto chunk_factor = 2; /* chunks per thread */

/* scores in bits */

constexpr auto k                          = 5;
static int merge_mindiagcount             = 4;
static double merge_minscore              = 16.0;
constexpr auto merge_dropmax         = 16.0;
constexpr auto merge_mismatchmax     = -4.0;

/* static variables */

static std::FILE * fp_fastqout = nullptr;
static std::FILE * fp_fastaout = nullptr;
static std::FILE * fp_fastqout_notmerged_fwd = nullptr;
static std::FILE * fp_fastqout_notmerged_rev = nullptr;
static std::FILE * fp_fastaout_notmerged_fwd = nullptr;
static std::FILE * fp_fastaout_notmerged_rev = nullptr;
static std::FILE * fp_eetabbedout = nullptr;

static fastx_handle fastq_fwd;
static fastx_handle fastq_rev;

static int64_t merged = 0;
static int64_t notmerged = 0;
static int64_t total = 0;

static double sum_read_length = 0.0;
static double sum_squared_fragment_length = 0.0;
static double sum_fragment_length = 0.0;

static pthread_t * pthread;
static pthread_attr_t attr;

constexpr auto n_quality_symbols = 128U;
std::array<std::array<char, n_quality_symbols>, n_quality_symbols> merge_qual_same {{}};
std::array<std::array<char, n_quality_symbols>, n_quality_symbols> merge_qual_diff {{}};
std::array<std::array<double, n_quality_symbols>, n_quality_symbols> match_score {{}};
std::array<std::array<double, n_quality_symbols>, n_quality_symbols> mism_score {{}};
std::array<double, n_quality_symbols> q2p {{}};

static double sum_ee_fwd = 0.0;
static double sum_ee_rev = 0.0;
static double sum_ee_merged = 0.0;
static uint64_t sum_errors_fwd = 0.0;
static uint64_t sum_errors_rev = 0.0;

static uint64_t failed_undefined = 0;
static uint64_t failed_minlen = 0;
static uint64_t failed_maxlen = 0;
static uint64_t failed_maxns = 0;
static uint64_t failed_minovlen = 0;
static uint64_t failed_maxdiffs = 0;
static uint64_t failed_maxdiffpct = 0;
static uint64_t failed_staggered = 0;
static uint64_t failed_indel = 0;
static uint64_t failed_repeat = 0;
static uint64_t failed_minmergelen = 0;
static uint64_t failed_maxmergelen = 0;
static uint64_t failed_maxee = 0;
static uint64_t failed_minscore = 0;
static uint64_t failed_nokmers = 0;

/* reasons for not merging:
   - undefined
   - ok
   - input seq too short (after truncation)
   - input seq too long
   - too many Ns in input
   - overlap too short
   - too many differences (maxdiffs)
   - too high percentage of differences (maxdiffpct)
   - staggered
   - indels in overlap region
   - potential repeats in overlap region / multiple overlaps
   - merged sequence too short
   - merged sequence too long
   - expected error too high
   - alignment score too low, insignificant, potential indel
   - too few kmers on same diag found
*/

enum struct Reason : char
  {
    undefined,
    ok,
    minlen,
    maxlen,
    maxns,
    minovlen,
    maxdiffs,
    maxdiffpct,
    staggered,
    indel,
    repeat,
    minmergelen,
    maxmergelen,
    maxee,
    minscore,
    nokmers
  };

enum struct State: char
  {
    empty,
    filled,
    inprogress,
    processed
  };

struct merge_data_s
{
  char * fwd_header = nullptr;
  char * rev_header = nullptr;
  char * fwd_sequence = nullptr;
  char * rev_sequence = nullptr;
  char * fwd_quality = nullptr;
  char * rev_quality = nullptr;
  int64_t header_alloc = 0;
  int64_t seq_alloc = 0;
  int64_t fwd_length = 0;
  int64_t rev_length = 0;
  int64_t fwd_trunc = 0;
  int64_t rev_trunc = 0;
  int64_t pair_no = 0;
  char * merged_sequence = nullptr;
  char * merged_quality = nullptr;
  int64_t merged_length = 0;
  int64_t merged_seq_alloc = 0;
  double ee_merged = 0;
  double ee_fwd = 0;
  double ee_rev = 0;
  int64_t fwd_errors = 0;
  int64_t rev_errors = 0;
  int64_t offset = 0;
  bool merged = false;
  Reason reason = Reason::undefined;
  State state = State::empty;
};

using merge_data_t = struct merge_data_s;

struct chunk_s
{
  int size = 0; /* size of merge_data = number of pairs of reads */
  State state = State::empty; /* state of chunk: empty, read, processed */
  std::vector<struct merge_data_s> merge_data_v;
  merge_data_t * merge_data = nullptr; /* data for merging */
};

using chunk_t = struct chunk_s;

static chunk_t * chunks; /* pointer to array of chunks */

static int chunk_count;
static int chunk_read_next;
static int chunk_process_next;
static int chunk_write_next;
static bool finished_reading = false;
static bool finished_all = false;
static int pairs_read = 0;
static int pairs_written = 0;

static pthread_mutex_t mutex_chunks;
static pthread_cond_t cond_chunks;


// refactoring: make generic function (extract to utils/file_open_write.cpp)
auto fileopenw(char const * filename) -> std::FILE *
{
  assert(filename != nullptr);
  auto * output_handle = fopen_output(filename);
  if (output_handle == nullptr)
    {
      fatal("Unable to open file for writing (%s)", filename);
    }
  return output_handle;
}


inline auto get_qual(char const quality_symbol) -> int
{
  assert(quality_symbol >= 33);
  assert(quality_symbol <= 126);

  auto const quality_value = static_cast<int>(quality_symbol - opt_fastq_ascii);

  if (quality_value < opt_fastq_qmin)
    {
      fprintf(stderr,
              "\n\nFatal error: FASTQ quality value (%d) below qmin (%"
              PRId64 ")\n",
              quality_value, opt_fastq_qmin);
      if (fp_log != nullptr)
        {
          fprintf(stderr,
                  "\n\nFatal error: FASTQ quality value (%d) below qmin (%"
                  PRId64 ")\n",
                  quality_value, opt_fastq_qmin);
        }
      exit(EXIT_FAILURE);
    }
  else if (quality_value > opt_fastq_qmax)
    {
      fprintf(stderr,
              "\n\nFatal error: FASTQ quality value (%d) above qmax (%"
              PRId64 ")\n",
              quality_value, opt_fastq_qmax);
      fprintf(stderr,
              "By default, quality values range from 0 to 41.\n"
              "To allow higher quality values, "
              "please use the option --fastq_qmax %d\n", quality_value);
      if (fp_log != nullptr)
        {
          fprintf(fp_log,
                  "\n\nFatal error: FASTQ quality value (%d) above qmax (%"
                  PRId64 ")\n",
                  quality_value, opt_fastq_qmax);
          fprintf(fp_log,
                  "By default, quality values range from 0 to 41.\n"
                  "To allow higher quality values, "
                  "please use the option --fastq_qmax %d\n", quality_value);
        }
      exit(EXIT_FAILURE);
    }
  return quality_value;
}


inline auto q_to_p(int const quality_symbol) -> double
{
  static constexpr auto low_quality_threshold = 2;
  static constexpr auto max_probability = 0.75;
  static constexpr auto quality_divider = 10.0;
  static constexpr auto power_base = 10.0;

  assert(quality_symbol >= 33);
  assert(quality_symbol <= 126);

  auto const quality_value = static_cast<int>(quality_symbol - opt_fastq_ascii);

  // refactor: extract branch to a separate operation
  if (quality_value < low_quality_threshold) {
    return max_probability;
  }
  // probability = 10^-(quality / 10)
  return std::pow(power_base, -quality_value / quality_divider);
}


auto precompute_qual() -> void
{
  /* Precompute tables of scores etc */
  auto const qmaxout = static_cast<double>(opt_fastq_qmaxout);
  auto const qminout = static_cast<double>(opt_fastq_qminout);

  for (auto x = 33; x <= 126; x++)
    {
      auto const px = q_to_p(x);
      q2p[x] = px;

      for (auto y = 33; y <= 126; y++)
        {
          auto const py = q_to_p(y);

          auto p = 0.0;
          auto q = 0.0;

          /* Quality score equations from Edgar & Flyvbjerg (2015) */

          /* Match */
          p = px * py / 3.0 / (1.0 - px - py + 4.0 * px * py / 3.0);
          q = std::round(-10.0 * std::log10(p));
          q = std::min(q, qmaxout);
          q = std::max(q, qminout);
          merge_qual_same[x][y] = opt_fastq_ascii + q;

          /* Mismatch, x is highest quality */
          p = px * (1.0 - py / 3.0) / (px + py - 4.0 * px * py / 3.0);
          q = std::round(-10.0 * std::log10(p));
          q = std::min(q, qmaxout);
          q = std::max(q, qminout);
          merge_qual_diff[x][y] = opt_fastq_ascii + q;

          /*
            observed match,
            p = probability that they truly are identical,
            given error probabilites of px and py, resp.
          */

          // Given two initially identical aligned bases, and
          // the error probabilities px and py,
          // what is the probability of observing a match (or a mismatch)?

          p = 1.0 - px - py + (px * py * 4.0 / 3.0);
          match_score[x][y] = std::log2(p / 0.25);

          // Use a minimum mismatch penalty

          mism_score[x][y] = std::min(std::log2((1.0 - p) / 0.75), merge_mismatchmax);
        }
    }
}


auto merge_sym(char * sym,       char * qual,
               char fwd_sym,     char rev_sym,
               char fwd_qual,    char rev_qual) -> void
{
  if (rev_sym == 'N')
    {
      * sym = fwd_sym;
      * qual = fwd_qual;
    }
  else if (fwd_sym == 'N')
    {
      * sym = rev_sym;
      * qual = rev_qual;
    }
  else if (fwd_sym == rev_sym)
    {
      /* agreement */
      * sym = fwd_sym;
      * qual = merge_qual_same[(unsigned) fwd_qual][(unsigned) rev_qual];
    }
  else
    {
      /* disagreement */
      if (fwd_qual > rev_qual)
        {
          * sym = fwd_sym;
          * qual = merge_qual_diff[(unsigned) fwd_qual][(unsigned) rev_qual];
        }
      else
        {
          * sym = rev_sym;
          * qual = merge_qual_diff[(unsigned) rev_qual][(unsigned) fwd_qual];
        }
    }
}


auto keep(merge_data_t * a_read_pair) -> void
{
  ++merged;

  sum_fragment_length += a_read_pair->merged_length;
  sum_squared_fragment_length += a_read_pair->merged_length * a_read_pair->merged_length;

  sum_ee_merged += a_read_pair->ee_merged;
  sum_ee_fwd += a_read_pair->ee_fwd;
  sum_ee_rev += a_read_pair->ee_rev;
  sum_errors_fwd += a_read_pair->fwd_errors;
  sum_errors_rev += a_read_pair->rev_errors;

  if (opt_fastqout != nullptr)
    {
      fastq_print_general(fp_fastqout,
                          a_read_pair->merged_sequence,
                          a_read_pair->merged_length,
                          a_read_pair->fwd_header,
                          strlen(a_read_pair->fwd_header),
                          a_read_pair->merged_quality,
                          0,
                          merged,
                          a_read_pair->ee_merged);
    }

  if (opt_fastaout != nullptr)
    {
      fasta_print_general(fp_fastaout,
                          nullptr,
                          a_read_pair->merged_sequence,
                          a_read_pair->merged_length,
                          a_read_pair->fwd_header,
                          strlen(a_read_pair->fwd_header),
                          0,
                          merged,
                          a_read_pair->ee_merged,
                          -1,
                          -1,
                          nullptr,
                          0.0);
    }

  if (opt_eetabbedout != nullptr)
    {
      fprintf(fp_eetabbedout, "%.2lf\t%.2lf\t%" PRId64 "\t%" PRId64 "\n",
              a_read_pair->ee_fwd, a_read_pair->ee_rev, a_read_pair->fwd_errors, a_read_pair->rev_errors);
    }
}


auto discard(merge_data_t * a_read_pair) -> void
{
  switch(a_read_pair->reason)
    {
    case Reason::undefined:
      ++failed_undefined;
      break;

    case Reason::ok:
      break;

    case Reason::minlen:
      ++failed_minlen;
      break;

    case Reason::maxlen:
      ++failed_maxlen;
      break;

    case Reason::maxns:
      ++failed_maxns;
      break;

    case Reason::minovlen:
      ++failed_minovlen;
      break;

    case Reason::maxdiffs:
      ++failed_maxdiffs;
      break;

    case Reason::maxdiffpct:
      ++failed_maxdiffpct;
      break;

    case Reason::staggered:
      ++failed_staggered;
      break;

    case Reason::indel:
      ++failed_indel;
      break;

    case Reason::repeat:
      ++failed_repeat;
      break;

    case Reason::minmergelen:
      ++failed_minmergelen;
      break;

    case Reason::maxmergelen:
      ++failed_maxmergelen;
      break;

    case Reason::maxee:
      ++failed_maxee;
      break;

    case Reason::minscore:
      ++failed_minscore;
      break;

    case Reason::nokmers:
      ++failed_nokmers;
      break;
    }

  ++notmerged;

  if (opt_fastqout_notmerged_fwd != nullptr)
    {
      fastq_print_general(fp_fastqout_notmerged_fwd,
                          a_read_pair->fwd_sequence,
                          a_read_pair->fwd_length,
                          a_read_pair->fwd_header,
                          strlen(a_read_pair->fwd_header),
                          a_read_pair->fwd_quality,
                          0,
                          notmerged,
                          -1.0);
    }

  if (opt_fastqout_notmerged_rev != nullptr)
    {
      fastq_print_general(fp_fastqout_notmerged_rev,
                          a_read_pair->rev_sequence,
                          a_read_pair->rev_length,
                          a_read_pair->rev_header,
                          strlen(a_read_pair->rev_header),
                          a_read_pair->rev_quality,
                          0,
                          notmerged,
                          -1.0);
    }

  if (opt_fastaout_notmerged_fwd != nullptr)
    {
      fasta_print_general(fp_fastaout_notmerged_fwd,
                          nullptr,
                          a_read_pair->fwd_sequence,
                          a_read_pair->fwd_length,
                          a_read_pair->fwd_header,
                          strlen(a_read_pair->fwd_header),
                          0,
                          notmerged,
                          -1.0,
                          -1, -1,
                          nullptr, 0.0);
    }

  if (opt_fastaout_notmerged_rev != nullptr)
    {
      fasta_print_general(fp_fastaout_notmerged_rev,
                          nullptr,
                          a_read_pair->rev_sequence,
                          a_read_pair->rev_length,
                          a_read_pair->rev_header,
                          strlen(a_read_pair->rev_header),
                          0,
                          notmerged,
                          -1.0,
                          -1, -1,
                          nullptr, 0.0);
    }
}


auto merge(merge_data_t * a_read_pair) -> void
{
  /* length of 5' overhang of the forward sequence not merged
     with the reverse sequence */

  auto const fwd_5prime_overhang = (a_read_pair->fwd_trunc > a_read_pair->offset) ?
    a_read_pair->fwd_trunc - a_read_pair->offset : 0;

  // reset struct members
  a_read_pair->ee_merged = 0.0;
  a_read_pair->ee_fwd = 0.0;
  a_read_pair->ee_rev = 0.0;
  a_read_pair->fwd_errors = 0;
  a_read_pair->rev_errors = 0;
  auto sym = '\0';
  auto qual = '\0';
  auto fwd_sym = '\0';
  auto fwd_qual = '\0';
  auto rev_sym = '\0';
  auto rev_qual = '\0';
  int64_t fwd_pos = 0;
  int64_t rev_pos = 0;
  int64_t merged_pos = 0;
  auto ee = 0.0;

  merged_pos = 0;

  // 5' overhang in forward sequence

  fwd_pos = 0;

  while (fwd_pos < fwd_5prime_overhang)
    {
      sym = a_read_pair->fwd_sequence[fwd_pos];
      qual = a_read_pair->fwd_quality[fwd_pos];

      a_read_pair->merged_sequence[merged_pos] = sym;
      a_read_pair->merged_quality[merged_pos] = qual;

      ee = q2p[(unsigned) qual];
      a_read_pair->ee_merged += ee;
      a_read_pair->ee_fwd += ee;

      ++fwd_pos;
      ++merged_pos;
    }

  // Merged region

  auto const rev_3prime_overhang = a_read_pair->offset > a_read_pair->fwd_trunc ?
    a_read_pair->offset - a_read_pair->fwd_trunc : 0;

  rev_pos = a_read_pair->rev_trunc - 1 - rev_3prime_overhang;

  while ((fwd_pos < a_read_pair->fwd_trunc) && (rev_pos >= 0))
    {
      fwd_sym = a_read_pair->fwd_sequence[fwd_pos];
      rev_sym = chrmap_complement[(int) (a_read_pair->rev_sequence[rev_pos])];
      fwd_qual = a_read_pair->fwd_quality[fwd_pos];
      rev_qual = a_read_pair->rev_quality[rev_pos];

      merge_sym(& sym,
                & qual,
                fwd_qual < 2 ? 'N' : fwd_sym,
                rev_qual < 2 ? 'N' : rev_sym,
                fwd_qual,
                rev_qual);

      if (sym != fwd_sym)
        {
          ++a_read_pair->fwd_errors;
        }
      if (sym != rev_sym)
        {
          ++a_read_pair->rev_errors;
        }

      a_read_pair->merged_sequence[merged_pos] = sym;
      a_read_pair->merged_quality[merged_pos] = qual;
      a_read_pair->ee_merged += q2p[(unsigned) qual];
      a_read_pair->ee_fwd += q2p[(unsigned) fwd_qual];
      a_read_pair->ee_rev += q2p[(unsigned) rev_qual];

      ++fwd_pos;
      --rev_pos;
      ++merged_pos;
    }

  // 5' overhang in reverse sequence

  while (rev_pos >= 0)
    {
      sym = chrmap_complement[(int) (a_read_pair->rev_sequence[rev_pos])];
      qual = a_read_pair->rev_quality[rev_pos];

      a_read_pair->merged_sequence[merged_pos] = sym;
      a_read_pair->merged_quality[merged_pos] = qual;
      ++merged_pos;

      ee = q2p[(unsigned) qual];
      a_read_pair->ee_merged += ee;
      a_read_pair->ee_rev += ee;

      --rev_pos;
    }

  auto const mergelen = merged_pos;
  a_read_pair->merged_length = mergelen;

  a_read_pair->merged_sequence[mergelen] = 0;
  a_read_pair->merged_quality[mergelen] = 0;

  if (a_read_pair->ee_merged <= opt_fastq_maxee)
    {
      a_read_pair->reason = Reason::ok;
      a_read_pair->merged = true;
    }
  else
    {
      a_read_pair->reason = Reason::maxee;
    }
}


auto optimize(merge_data_t * a_read_pair,
              kh_handle_s * kmerhash) -> int64_t
{
  /* ungapped alignment in each diagonal */

  int64_t const i1 = 1;
  auto const i2 = a_read_pair->fwd_trunc + a_read_pair->rev_trunc - 1;

  auto best_score = 0.0;
  int64_t best_i = 0;
  int64_t best_diffs = 0;

  auto hits = 0;

  auto kmers = 0;

  std::vector<int> diags(a_read_pair->fwd_trunc + a_read_pair->rev_trunc, 0);

  kh_insert_kmers(kmerhash, k, a_read_pair->fwd_sequence, a_read_pair->fwd_trunc);
  kh_find_diagonals(kmerhash, k, a_read_pair->rev_sequence, a_read_pair->rev_trunc, diags.data());

  for (int64_t i = i1; i <= i2; i++)
    {
      int const diag = a_read_pair->rev_trunc + a_read_pair->fwd_trunc - i;
      auto const diagcount = diags[diag];

      if (diagcount >= merge_mindiagcount)
        {
          kmers = 1;

          /* for each interesting diagonal */

          auto const fwd_3prime_overhang
            = i > a_read_pair->rev_trunc ? i - a_read_pair->rev_trunc : 0;
          auto const rev_3prime_overhang
            = i > a_read_pair->fwd_trunc ? i - a_read_pair->fwd_trunc : 0;
          auto const overlap
            = i - fwd_3prime_overhang - rev_3prime_overhang;
          auto const fwd_pos_start
            = a_read_pair->fwd_trunc - fwd_3prime_overhang - 1;
          auto const rev_pos_start
            = a_read_pair->rev_trunc - rev_3prime_overhang - overlap;

          auto fwd_pos = fwd_pos_start;
          auto rev_pos = rev_pos_start;
          auto score = 0.0;

          int64_t diffs = 0;
          auto score_high = 0.0;
          auto dropmax = 0.0;

          for (int64_t j = 0; j < overlap; j++)
            {
              /* for each pair of bases in the overlap */

              auto const fwd_sym = a_read_pair->fwd_sequence[fwd_pos];
              auto const rev_sym = chrmap_complement[(int) (a_read_pair->rev_sequence[rev_pos])];

              unsigned int const fwd_qual = a_read_pair->fwd_quality[fwd_pos];
              unsigned int const rev_qual = a_read_pair->rev_quality[rev_pos];

              --fwd_pos;
              ++rev_pos;

              if (fwd_sym == rev_sym)
                {
                  score += match_score[fwd_qual][rev_qual];
                  score_high = std::max(score, score_high);
                }
              else
                {
                  score += mism_score[fwd_qual][rev_qual];
                  ++diffs;
                  if (score < score_high - dropmax)
                    {
                      dropmax = score_high - score;
                    }
                }
            }

          if (dropmax >= merge_dropmax)
            {
              score = 0.0;
            }

          if (score >= merge_minscore)
            {
              ++hits;
            }

          if (score > best_score)
            {
              best_score = score;
              best_i = i;
              best_diffs = diffs;
            }
        }
    }

  if (hits > 1)
    {
      a_read_pair->reason = Reason::repeat;
      return 0;
    }

  if ((! opt_fastq_allowmergestagger) && (best_i > a_read_pair->fwd_trunc))
    {
      a_read_pair->reason = Reason::staggered;
      return 0;
    }

  if (best_diffs > opt_fastq_maxdiffs)
    {
      a_read_pair->reason = Reason::maxdiffs;
      return 0;
    }

  if ((100.0 * best_diffs / best_i) > opt_fastq_maxdiffpct)
    {
      a_read_pair->reason = Reason::maxdiffpct;
      return 0;
    }

  if (kmers == 0)
    {
      a_read_pair->reason = Reason::nokmers;
      return 0;
    }

  if (best_score < merge_minscore)
    {
      a_read_pair->reason = Reason::minscore;
      return 0;
    }

  if (best_i < opt_fastq_minovlen)
    {
      a_read_pair->reason = Reason::minovlen;
      return 0;
    }

  int const mergelen = a_read_pair->fwd_trunc + a_read_pair->rev_trunc - best_i;

  if (mergelen < opt_fastq_minmergelen)
    {
      a_read_pair->reason = Reason::minmergelen;
      return 0;
    }

  if (mergelen > opt_fastq_maxmergelen)
    {
      a_read_pair->reason = Reason::maxmergelen;
      return 0;
    }

  return best_i;
}


auto process(merge_data_t * a_read_pair,
             struct kh_handle_s * kmerhash) -> void
{
  a_read_pair->merged = false;

  auto skip = false;

  /* check length */

  if ((a_read_pair->fwd_length < opt_fastq_minlen) ||
      (a_read_pair->rev_length < opt_fastq_minlen))
    {
      a_read_pair->reason = Reason::minlen;
      skip = true;
    }

  if ((a_read_pair->fwd_length > opt_fastq_maxlen) ||
      (a_read_pair->rev_length > opt_fastq_maxlen))
    {
      a_read_pair->reason = Reason::maxlen;
      skip = true;
    }

  /* truncate sequences by quality */

  int64_t fwd_trunc = a_read_pair->fwd_length;

  if (! skip)
    {
      for (int64_t i = 0; i < a_read_pair->fwd_length; i++)
        {
          if (get_qual(a_read_pair->fwd_quality[i]) <= opt_fastq_truncqual)
            {
              fwd_trunc = i;
              break;
            }
        }
      if (fwd_trunc < opt_fastq_minlen)
        {
          a_read_pair->reason = Reason::minlen;
          skip = true;
        }
    }

  a_read_pair->fwd_trunc = fwd_trunc;

  auto rev_trunc = a_read_pair->rev_length;

  if (! skip)
    {
      for (int64_t i = 0; i < a_read_pair->rev_length; i++)
        {
          if (get_qual(a_read_pair->rev_quality[i]) <= opt_fastq_truncqual)
            {
              rev_trunc = i;
              break;
            }
        }
      if (rev_trunc < opt_fastq_minlen)
        {
          a_read_pair->reason = Reason::minlen;
          skip = true;
        }
    }

  a_read_pair->rev_trunc = rev_trunc;

  /* count n's */

  /* replace quality of N's by zero */

  if (! skip)
    {
      int64_t fwd_ncount = 0;
      for (int64_t i = 0; i < fwd_trunc; i++)
        {
          if (a_read_pair->fwd_sequence[i] == 'N')
            {
              a_read_pair->fwd_quality[i] = opt_fastq_ascii;
              ++fwd_ncount;
            }
        }
      if (fwd_ncount > opt_fastq_maxns)
        {
          a_read_pair->reason = Reason::maxns;
          skip = true;
        }
    }

  if (! skip)
    {
      int64_t rev_ncount = 0;
      for (int64_t i = 0; i < rev_trunc; i++)
        {
          if (a_read_pair->rev_sequence[i] == 'N')
            {
              a_read_pair->rev_quality[i] = opt_fastq_ascii;
              ++rev_ncount;
            }
        }
      if (rev_ncount > opt_fastq_maxns)
        {
          a_read_pair->reason = Reason::maxns;
          skip = true;
        }
    }

  a_read_pair->offset = 0;

  if (! skip)
    {
      a_read_pair->offset = optimize(a_read_pair, kmerhash);
    }

  if (a_read_pair->offset > 0)
    {
      merge(a_read_pair);
    }

  a_read_pair->state = State::processed;
}


auto read_pair(merge_data_t * a_read_pair) -> bool
{
  if (fastq_next(fastq_fwd, false, chrmap_upcase))
    {
      if (! fastq_next(fastq_rev, false, chrmap_upcase))
        {
          fatal("More forward reads than reverse reads");
        }

      /* allocate more memory if necessary */

      int64_t const fwd_header_len = fastq_get_header_length(fastq_fwd);
      int64_t const rev_header_len = fastq_get_header_length(fastq_rev);
      int64_t const header_needed = std::max(fwd_header_len, rev_header_len) + 1;

      if (header_needed > a_read_pair->header_alloc)
        {
          a_read_pair->header_alloc = header_needed;
          a_read_pair->fwd_header = (char *) xrealloc(a_read_pair->fwd_header, header_needed);
          a_read_pair->rev_header = (char *) xrealloc(a_read_pair->rev_header, header_needed);
        }

      a_read_pair->fwd_length = fastq_get_sequence_length(fastq_fwd);
      a_read_pair->rev_length = fastq_get_sequence_length(fastq_rev);
      int64_t const seq_needed = std::max(a_read_pair->fwd_length, a_read_pair->rev_length) + 1;

      sum_read_length += a_read_pair->fwd_length + a_read_pair->rev_length;

      if (seq_needed > a_read_pair->seq_alloc)
        {
          a_read_pair->seq_alloc = seq_needed;
          a_read_pair->fwd_sequence = (char *) xrealloc(a_read_pair->fwd_sequence, seq_needed);
          a_read_pair->rev_sequence = (char *) xrealloc(a_read_pair->rev_sequence, seq_needed);
          a_read_pair->fwd_quality  = (char *) xrealloc(a_read_pair->fwd_quality,  seq_needed);
          a_read_pair->rev_quality  = (char *) xrealloc(a_read_pair->rev_quality,  seq_needed);
        }


      int64_t const merged_seq_needed = a_read_pair->fwd_length + a_read_pair->rev_length + 1;

      if (merged_seq_needed > a_read_pair->merged_seq_alloc)
        {
          a_read_pair->merged_seq_alloc = merged_seq_needed;
          a_read_pair->merged_sequence = (char *) xrealloc(a_read_pair->merged_sequence,
                                                 merged_seq_needed);
          a_read_pair->merged_quality = (char *) xrealloc(a_read_pair->merged_quality,
                                                merged_seq_needed);
        }

      /* make local copies of the seq, header and qual */

      strcpy(a_read_pair->fwd_header,   fastq_get_header(fastq_fwd));
      strcpy(a_read_pair->rev_header,   fastq_get_header(fastq_rev));
      strcpy(a_read_pair->fwd_sequence, fastq_get_sequence(fastq_fwd));
      strcpy(a_read_pair->rev_sequence, fastq_get_sequence(fastq_rev));
      strcpy(a_read_pair->fwd_quality,  fastq_get_quality(fastq_fwd));
      strcpy(a_read_pair->rev_quality,  fastq_get_quality(fastq_rev));

      a_read_pair->merged_sequence[0] = 0;
      a_read_pair->merged_quality[0] = 0;
      a_read_pair->merged = false;
      a_read_pair->pair_no = total++;

      return true;
    }
  return false;
}


auto keep_or_discard(merge_data_t * a_read_pair) -> void
{
  if (a_read_pair->merged)
    {
      keep(a_read_pair);
    }
  else
    {
      discard(a_read_pair);
    }
}


auto free_merge_data(merge_data_t & a_read_pair) -> void
{
  if (a_read_pair.fwd_header != nullptr)
    {
      xfree(a_read_pair.fwd_header);
    }
  if (a_read_pair.rev_header != nullptr)
    {
      xfree(a_read_pair.rev_header);
    }
  if (a_read_pair.fwd_sequence != nullptr)
    {
      xfree(a_read_pair.fwd_sequence);
    }
  if (a_read_pair.rev_sequence != nullptr)
    {
      xfree(a_read_pair.rev_sequence);
    }
  if (a_read_pair.fwd_quality != nullptr)
    {
      xfree(a_read_pair.fwd_quality);
    }
  if (a_read_pair.rev_quality != nullptr)
    {
      xfree(a_read_pair.rev_quality);
    }
  if (a_read_pair.merged_sequence != nullptr)
    {
      xfree(a_read_pair.merged_sequence);
    }
  if (a_read_pair.merged_quality != nullptr)
    {
      xfree(a_read_pair.merged_quality);
    }
}


inline auto chunk_perform_read() -> void
{
  while ((! finished_reading) && (chunks[chunk_read_next].state == State::empty))
    {
      xpthread_mutex_unlock(&mutex_chunks);
      progress_update(fastq_get_position(fastq_fwd));
      auto r = 0;
      while ((r < chunk_size) &&
             read_pair(chunks[chunk_read_next].merge_data + r))
        {
          ++r;
        }
      chunks[chunk_read_next].size = r;
      xpthread_mutex_lock(&mutex_chunks);
      pairs_read += r;
      if (r > 0)
        {
          chunks[chunk_read_next].state = State::filled;
          chunk_read_next = (chunk_read_next + 1) % chunk_count;
        }
      if (r < chunk_size)
        {
          finished_reading = true;
          if (pairs_written >= pairs_read)
            {
              finished_all = true;
            }
        }
      xpthread_cond_broadcast(&cond_chunks);
    }
}


inline auto chunk_perform_write() -> void
{
  while (chunks[chunk_write_next].state == State::processed)
    {
      xpthread_mutex_unlock(&mutex_chunks);
      for (auto i = 0; i < chunks[chunk_write_next].size; i++)
        {
          keep_or_discard(chunks[chunk_write_next].merge_data + i);
        }
      xpthread_mutex_lock(&mutex_chunks);
      pairs_written += chunks[chunk_write_next].size;
      chunks[chunk_write_next].state = State::empty;
      if (finished_reading && (pairs_written >= pairs_read))
        {
          finished_all = true;
        }
      chunk_write_next = (chunk_write_next + 1) % chunk_count;
      xpthread_cond_broadcast(&cond_chunks);
    }
}


inline auto chunk_perform_process(struct kh_handle_s * kmerhash) -> void
{
  auto const chunk_current = chunk_process_next;
  if (chunks[chunk_current].state == State::filled)
    {
      chunks[chunk_current].state = State::inprogress;
      chunk_process_next = (chunk_current + 1) % chunk_count;
      xpthread_cond_broadcast(&cond_chunks);
      xpthread_mutex_unlock(&mutex_chunks);
      for (auto i = 0; i < chunks[chunk_current].size; i++)
        {
          process(chunks[chunk_current].merge_data + i, kmerhash);
        }
      xpthread_mutex_lock(&mutex_chunks);
      chunks[chunk_current].state = State::processed;
      xpthread_cond_broadcast(&cond_chunks);
    }
}


auto pair_worker(void * vp) -> void *
{
  /* new */

  auto t = (int64_t) vp;

  auto * kmerhash = kh_init();

  xpthread_mutex_lock(&mutex_chunks);

  while (! finished_all)
    {
      if (opt_threads == 1)
        {
          /* One thread does it all */
          chunk_perform_read();
          chunk_perform_process(kmerhash);
          chunk_perform_write();
        }
      else if (opt_threads == 2)
        {
          if (t == 0)
            {
              /* first thread reads and processes */
              while (!
                     (
                      finished_all
                      ||
                      (chunks[chunk_process_next].state == State::filled)
                      ||
                      ((! finished_reading) &&
                       chunks[chunk_read_next].state == State::empty)))
                {
                  xpthread_cond_wait(&cond_chunks, &mutex_chunks);
                }

              chunk_perform_read();
              chunk_perform_process(kmerhash);
            }
          else /* t == 1 */
            {
              /* second thread writes and processes */
              while (!
                     (
                      finished_all
                      ||
                      (chunks[chunk_process_next].state == State::filled)
                      ||
                      (chunks[chunk_write_next].state == State::processed)
                      )
                     )
                {
                  xpthread_cond_wait(&cond_chunks, &mutex_chunks);
                }

              chunk_perform_write();
              chunk_perform_process(kmerhash);
            }
        }
      else
        {
          if (t == 0)
            {
              /* first thread reads and processes */
              while (!
                     (
                      finished_all
                      ||
                      ((! finished_reading) &&
                       (chunks[chunk_read_next].state == State::empty))
                      ||
                      (chunks[chunk_process_next].state == State::filled)
                      )
                     )
                {
                  xpthread_cond_wait(&cond_chunks, &mutex_chunks);
                }

              chunk_perform_read();
              chunk_perform_process(kmerhash);
            }
          else if (t == opt_threads - 1)
            {
              /* last thread writes and processes */
              while (!
                     (
                      finished_all
                      ||
                      (chunks[chunk_write_next].state == State::processed)
                      ||
                      (chunks[chunk_process_next].state == State::filled)
                      )
                     )
                {
                  xpthread_cond_wait(&cond_chunks, &mutex_chunks);
                }

              chunk_perform_write();
              chunk_perform_process(kmerhash);
            }
          else
            {
              /* the other threads are only processing */
              while (!
                     (
                      finished_all
                      ||
                      (chunks[chunk_process_next].state == State::filled)
                      )
                     )
                {
                  xpthread_cond_wait(&cond_chunks, &mutex_chunks);
                }

              chunk_perform_process(kmerhash);
            }
        }
    }

  xpthread_mutex_unlock(&mutex_chunks);

  kh_exit(kmerhash);

  return nullptr;
}


auto pair_all() -> void
{
  /* prepare chunks */

  chunk_count = chunk_factor * opt_threads;
  chunk_read_next = 0;
  chunk_process_next = 0;
  chunk_write_next = 0;

  std::vector<chunk_t> chunks_v(chunk_count);
  chunks = chunks_v.data();

  for (auto & a_chunk: chunks_v) {
    a_chunk.merge_data_v.resize(chunk_size);
    a_chunk.merge_data = a_chunk.merge_data_v.data();
  }

  xpthread_mutex_init(&mutex_chunks, nullptr);
  xpthread_cond_init(&cond_chunks, nullptr);

  /* prepare threads */

  xpthread_attr_init(&attr);
  xpthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
  std::vector<pthread_t> pthread_v(opt_threads);
  pthread = pthread_v.data();

  for (auto t = 0; t < opt_threads; t++)
    {
      xpthread_create(&pthread_v[t], &attr, pair_worker, (void *) (int64_t) t);
    }

  /* wait for threads to terminate */

  for (auto t = 0; t < opt_threads; t++)
    {
      xpthread_join(pthread_v[t], nullptr);
    }

  /* free threads */

  xpthread_attr_destroy(&attr);

  /* free chunks */

  xpthread_cond_destroy(&cond_chunks);
  xpthread_mutex_destroy(&mutex_chunks);

  for (auto & a_chunk: chunks_v) {
      for (auto & a_read_pair: a_chunk.merge_data_v) {
        free_merge_data(a_read_pair);
      }
      a_chunk.merge_data = nullptr;
  }
  chunks = nullptr;
}


auto print_stats(std::FILE * output_handle) -> void
{
  fprintf(output_handle,
          "%10" PRIu64 "  Pairs\n",
          total);

  fprintf(output_handle,
          "%10" PRIu64 "  Merged",
          merged);
  if (total > 0)
    {
      fprintf(output_handle,
              " (%.1lf%%)",
              100.0 * merged / total);
    }
  fprintf(output_handle, "\n");

  fprintf(output_handle,
          "%10" PRIu64 "  Not merged",
          notmerged);
  if (total > 0)
    {
      fprintf(output_handle,
              " (%.1lf%%)",
              100.0 * notmerged / total);
    }
  fprintf(output_handle, "\n");

  if (notmerged > 0)
    {
      fprintf(output_handle, "\nPairs that failed merging due to various reasons:\n");
    }

  if (failed_undefined != 0U)
    {
      fprintf(output_handle,
              "%10" PRIu64 "  undefined reason\n",
              failed_undefined);
    }

  if (failed_minlen != 0U)
    {
      fprintf(output_handle,
              "%10" PRIu64 "  reads too short (after truncation)\n",
              failed_minlen);
    }

  if (failed_maxlen != 0U)
    {
      fprintf(output_handle,
              "%10" PRIu64 "  reads too long (after truncation)\n",
              failed_maxlen);
    }

  if (failed_maxns != 0U)
    {
      fprintf(output_handle,
              "%10" PRIu64 "  too many N's\n",
              failed_maxns);
    }

  if (failed_nokmers != 0U)
    {
      fprintf(output_handle,
              "%10" PRIu64 "  too few kmers found on same diagonal\n",
              failed_nokmers);
    }

  if (failed_repeat != 0U)
    {
      fprintf(output_handle,
              "%10" PRIu64 "  multiple potential alignments\n",
              failed_repeat);
    }

  if (failed_maxdiffs != 0U)
    {
      fprintf(output_handle,
              "%10" PRIu64 "  too many differences\n",
              failed_maxdiffs);
    }

  if (failed_maxdiffpct != 0U)
    {
      fprintf(output_handle,
              "%10" PRIu64 "  too high percentage of differences\n",
              failed_maxdiffpct);
    }

  if (failed_minscore != 0U)
    {
      fprintf(output_handle,
              "%10" PRIu64 "  alignment score too low, or score drop too high\n",
              failed_minscore);
    }

  if (failed_minovlen != 0U)
    {
      fprintf(output_handle,
              "%10" PRIu64 "  overlap too short\n",
              failed_minovlen);
    }

  if (failed_maxee != 0U)
    {
      fprintf(output_handle,
              "%10" PRIu64 "  expected error too high\n",
              failed_maxee);
    }

  if (failed_minmergelen != 0U)
    {
      fprintf(output_handle,
              "%10" PRIu64 "  merged fragment too short\n",
              failed_minmergelen);
    }

  if (failed_maxmergelen != 0U)
    {
      fprintf(output_handle,
              "%10" PRIu64 "  merged fragment too long\n",
              failed_maxmergelen);
    }

  if (failed_staggered != 0U)
    {
      fprintf(output_handle,
              "%10" PRIu64 "  staggered read pairs\n",
              failed_staggered);
    }

  if (failed_indel != 0U)
    {
      fprintf(output_handle,
              "%10" PRIu64 "  indel errors\n",
              failed_indel);
    }

  fprintf(output_handle, "\n");

  if (total > 0)
    {
      fprintf(output_handle, "Statistics of all reads:\n");

      auto const mean_read_length = sum_read_length / (2.0 * pairs_read);

      fprintf(output_handle,
              "%10.2f  Mean read length\n",
              mean_read_length);
    }

  if (merged > 0)
    {
      fprintf(output_handle, "\n");

      fprintf(output_handle, "Statistics of merged reads:\n");

      auto const mean = sum_fragment_length / merged;

      fprintf(output_handle,
              "%10.2f  Mean fragment length\n",
              mean);

      auto const stdev = sqrt((sum_squared_fragment_length
                           - 2.0 * mean * sum_fragment_length
                           + mean * mean * merged)
                          / (merged + 0.0));

      fprintf(output_handle,
              "%10.2f  Standard deviation of fragment length\n",
              stdev);

      fprintf(output_handle,
              "%10.2f  Mean expected error in forward sequences\n",
              sum_ee_fwd / merged);

      fprintf(output_handle,
              "%10.2f  Mean expected error in reverse sequences\n",
              sum_ee_rev / merged);

      fprintf(output_handle,
              "%10.2f  Mean expected error in merged sequences\n",
              sum_ee_merged / merged);

      fprintf(output_handle,
              "%10.2f  Mean observed errors in merged region of forward sequences\n",
              1.0 * sum_errors_fwd / merged);

      fprintf(output_handle,
              "%10.2f  Mean observed errors in merged region of reverse sequences\n",
              1.0 * sum_errors_rev / merged);

      fprintf(output_handle,
              "%10.2f  Mean observed errors in merged region\n",
              1.0 * (sum_errors_fwd + sum_errors_rev) / merged);
    }
}


auto fastq_mergepairs(struct Parameters const & parameters) -> void
{
  /* fatal error if specified overlap is too small */

  if (opt_fastq_minovlen < 5)
    {
      fatal("Overlap specified with --fastq_minovlen must be at least 5");
    }

  /* relax default parameters in case of short overlaps */

  if (opt_fastq_minovlen < 9)
    {
      merge_mindiagcount = opt_fastq_minovlen - 4;
      merge_minscore = 1.6 * opt_fastq_minovlen;
    }

  /* open input files */

  fastq_fwd = fastq_open(parameters.opt_fastq_mergepairs);
  fastq_rev = fastq_open(opt_reverse);

  /* open output files */

  if (opt_fastqout != nullptr)
    {
      fp_fastqout = fileopenw(opt_fastqout);
    }
  if (opt_fastaout != nullptr)
    {
      fp_fastaout = fileopenw(opt_fastaout);
    }
  if (opt_fastqout_notmerged_fwd != nullptr)
    {
      fp_fastqout_notmerged_fwd = fileopenw(opt_fastqout_notmerged_fwd);
    }
  if (opt_fastqout_notmerged_rev != nullptr)
    {
      fp_fastqout_notmerged_rev = fileopenw(opt_fastqout_notmerged_rev);
    }
  if (opt_fastaout_notmerged_fwd != nullptr)
    {
      fp_fastaout_notmerged_fwd = fileopenw(opt_fastaout_notmerged_fwd);
    }
  if (opt_fastaout_notmerged_rev != nullptr)
    {
      fp_fastaout_notmerged_rev = fileopenw(opt_fastaout_notmerged_rev);
    }
  if (opt_eetabbedout != nullptr)
    {
      fp_eetabbedout = fileopenw(opt_eetabbedout);
    }

  /* precompute merged quality values */

  precompute_qual();

  /* main */

  uint64_t const filesize = fastq_get_size(fastq_fwd);
  progress_init("Merging reads", filesize);

  if (! fastq_fwd->is_empty)
    {
      pair_all();
    }

  progress_done();

  if (fastq_next(fastq_rev, true, chrmap_upcase))
    {
      fatal("More reverse reads than forward reads");
    }

  if (fp_log != nullptr) {
    print_stats(fp_log);
  }
  else {
    print_stats(stderr);
  }

  /* clean up */

  if (opt_eetabbedout != nullptr)
    {
      fclose(fp_eetabbedout);
    }
  if (opt_fastaout_notmerged_rev != nullptr)
    {
      fclose(fp_fastaout_notmerged_rev);
    }
  if (opt_fastaout_notmerged_fwd != nullptr)
    {
      fclose(fp_fastaout_notmerged_fwd);
    }
  if (opt_fastqout_notmerged_rev != nullptr)
    {
      fclose(fp_fastqout_notmerged_rev);
    }
  if (opt_fastqout_notmerged_fwd != nullptr)
    {
      fclose(fp_fastqout_notmerged_fwd);
    }
  if (opt_fastaout != nullptr)
    {
      fclose(fp_fastaout);
    }
  if (opt_fastqout != nullptr)
    {
      fclose(fp_fastqout);
    }

  fastq_close(fastq_rev);
  fastq_rev = nullptr;
  fastq_close(fastq_fwd);
  fastq_fwd = nullptr;
}
