/*

  VSEARCH5D: a modified version of VSEARCH

  Copyright (C) 2016-2021, Akifumi S. Tanabe

  Contact: Akifumi S. Tanabe
  https://github.com/astanabe/vsearch5d

  Original version of VSEARCH
  Copyright (C) 2014-2021, Torbjorn Rognes, Frederic Mahe and Tomas Flouri
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

#include "vsearch5d.h"

static uint64_t fragment_no = 0;
static uint64_t fragment_rev_no = 0;
static uint64_t fragment_discarded_no = 0;
static uint64_t fragment_discarded_rev_no = 0;

int cut_one(fastx_handle h,
            FILE * fp_fastaout,
            FILE * fp_fastaout_discarded,
            FILE * fp_fastaout_rev,
            FILE * fp_fastaout_discarded_rev,
            char * pattern,
            int pattern_length,
            int cut_fwd,
            int cut_rev)
{
  char * seq  = fasta_get_sequence(h);
  int seq_length = fasta_get_sequence_length(h);

  /* get reverse complement */
  char * rc = (char *) xmalloc(seq_length + 1);
  reverse_complement(rc, seq, seq_length);

  int frag_start = 0;
  int frag_length = seq_length;
  int matches = 0;

  int rc_start = seq_length;
  int rc_length = 0;

  for(int i = 0; i < seq_length - pattern_length + 1; i++)
    {
      bool match = true;
      for(int j = 0; j < pattern_length; j++)
        {
          if ((chrmap_4bit[(unsigned char)(pattern[j])] &
               chrmap_4bit[(unsigned char)(seq[i+j])]) == 0)
            {
              match = false;
              break;
            }
        }

      if (match)
        {
          matches++;

          frag_length = i + cut_fwd - frag_start;

          rc_length = rc_start - (seq_length - (i + cut_rev));
          rc_start -= rc_length;

          if (frag_length > 0)
            {
              if (opt_fastaout)
                {
                  fasta_print_general(fp_fastaout,
                                      nullptr,
                                      fasta_get_sequence(h) + frag_start,
                                      frag_length,
                                      fasta_get_header(h),
                                      fasta_get_header_length(h),
                                      fasta_get_abundance(h),
                                      ++fragment_no,
                                      -1.0,
                                      -1,
                                      -1,
                                      nullptr,
                                      0.0);
                }
            }

          if (rc_length > 0)
            {
              if (opt_fastaout_rev)
                {
                  fasta_print_general(fp_fastaout_rev,
                                      nullptr,
                                      rc + rc_start,
                                      rc_length,
                                      fasta_get_header(h),
                                      fasta_get_header_length(h),
                                      fasta_get_abundance(h),
                                      ++fragment_rev_no,
                                      -1.0,
                                      -1,
                                      -1,
                                      nullptr,
                                      0.0);
                }
            }

          frag_start += frag_length;
        }
    }

  if (matches > 0)
    {
      frag_length = seq_length - frag_start;

      if (frag_length > 0)
        {
          if (opt_fastaout)
            {
              fasta_print_general(fp_fastaout,
                                  nullptr,
                                  fasta_get_sequence(h) + frag_start,
                                  frag_length,
                                  fasta_get_header(h),
                                  fasta_get_header_length(h),
                                  fasta_get_abundance(h),
                                  ++fragment_no,
                                  -1.0,
                                  -1,
                                  -1,
                                  nullptr,
                                  0.0);
            }
        }

      rc_length = rc_start;
      rc_start = 0;

      if (rc_length > 0)
        {
          if (opt_fastaout_rev)
            {
              fasta_print_general(fp_fastaout_rev,
                                  nullptr,
                                  rc + rc_start,
                                  rc_length,
                                  fasta_get_header(h),
                                  fasta_get_header_length(h),
                                  fasta_get_abundance(h),
                                  ++fragment_rev_no,
                                  -1.0,
                                  -1,
                                  -1,
                                  nullptr,
                                  0.0);
            }
        }
    }
  else
    {
      if (opt_fastaout_discarded)
        {
          fasta_print_general(fp_fastaout_discarded,
                              nullptr,
                              fasta_get_sequence(h),
                              seq_length,
                              fasta_get_header(h),
                              fasta_get_header_length(h),
                              fasta_get_abundance(h),
                              ++fragment_discarded_no,
                              -1.0,
                              -1,
                              -1,
                              nullptr,
                              0.0);
        }

      if (opt_fastaout_discarded_rev)
        {
          fasta_print_general(fp_fastaout_discarded_rev,
                              nullptr,
                              rc,
                              seq_length,
                              fasta_get_header(h),
                              fasta_get_header_length(h),
                              fasta_get_abundance(h),
                              ++fragment_discarded_rev_no,
                              -1.0,
                              -1,
                              -1,
                              nullptr,
                              0.0);
        }
    }

  xfree(rc);

  return matches;
}

void cut()
{
  if ((!opt_fastaout) &&
      (!opt_fastaout_discarded) &&
      (!opt_fastaout_rev) &&
      (!opt_fastaout_discarded_rev))
    {
      fatal("No output files specified");
    }

  fastx_handle h = nullptr;

  h = fasta_open(opt_cut);

  if (!h)
    {
      fatal("Unrecognized file type (not proper FASTA format)");
    }

  uint64_t filesize = fasta_get_size(h);

  FILE * fp_fastaout = nullptr;
  FILE * fp_fastaout_discarded = nullptr;
  FILE * fp_fastaout_rev = nullptr;
  FILE * fp_fastaout_discarded_rev = nullptr;

  if (opt_fastaout)
    {
      fp_fastaout = fopen_output(opt_fastaout);
      if (!fp_fastaout)
        {
          fatal("Unable to open FASTA output file for writing");
        }
    }

  if (opt_fastaout_rev)
    {
      fp_fastaout_rev = fopen_output(opt_fastaout_rev);
      if (!fp_fastaout_rev)
        {
          fatal("Unable to open FASTA output file for writing");
        }
    }

  if (opt_fastaout_discarded)
    {
      fp_fastaout_discarded = fopen_output(opt_fastaout_discarded);
      if (!fp_fastaout_discarded)
        {
          fatal("Unable to open FASTA output file for writing");
        }
    }

  if (opt_fastaout_discarded_rev)
    {
      fp_fastaout_discarded_rev = fopen_output(opt_fastaout_discarded_rev);
      if (!fp_fastaout_discarded_rev)
        {
          fatal("Unable to open FASTA output file for writing");
        }
    }

  char * pattern = opt_cut_pattern;

  if (pattern == nullptr)
    {
      fatal("No cut pattern string specified with --cut_pattern");
    }

  int n = strlen(pattern);

  if (n == 0)
    {
      fatal("Empty cut pattern string");
    }

  int cut_fwd = -1;
  int cut_rev = -1;

  int j = 0;
  for (int i = 0; i < n ; i++)
    {
      unsigned char x = pattern[i];
      if (x == '^')
        {
          if (j < 0)
            {
              fatal("Multiple cut sites not supported");

            }
          cut_fwd = j;
        }
      else if (x == '_')
        {
          if (j < 0)
            {
              fatal("Multiple cut sites not supported");

            }
          cut_rev = j;
        }
      else if (chrmap_4bit[(unsigned int)x])
        {
          pattern[j++] = x;
        }
      else
        {
          fatal("Illegal character in cut pattern");
        }
    }

  if (cut_fwd < 0)
    {
      fatal("No forward sequence cut site (^) found in pattern");
    }

  if (cut_rev < 0)
    {
      fatal("No reverse sequence cut site (_) found in pattern");
    }

  progress_init("Cutting sequences", filesize);

  int64_t cut = 0;
  int64_t uncut = 0;
  int64_t matches = 0;

  while(fasta_next(h, false, chrmap_no_change))
    {
      int64_t m = cut_one(h,
                          fp_fastaout,
                          fp_fastaout_discarded,
                          fp_fastaout_rev,
                          fp_fastaout_discarded_rev,
                          pattern,
                          n - 2,
                          cut_fwd,
                          cut_rev);
      matches += m;
      if (m > 0)
        {
          cut++;
        }
      else
        {
          uncut++;
        }

      progress_update(fasta_get_position(h));
    }

  progress_done();

  if (! opt_quiet)
    {
      fprintf(stderr,
              "%" PRId64 " sequence(s) cut %" PRId64 " times, %" PRId64 " sequence(s) never cut.\n",
              cut, matches, uncut);
    }

  if (opt_log)
    {
      fprintf(fp_log,
              "%" PRId64 " sequence(s) cut %" PRId64 " times, %" PRId64 " sequence(s) never cut.\n",
              cut, matches, uncut);
    }

  if (opt_fastaout)
    {
      fclose(fp_fastaout);
    }

  if (opt_fastaout_rev)
    {
      fclose(fp_fastaout_rev);
    }

  if (opt_fastaout_discarded)
    {
      fclose(fp_fastaout_discarded);
    }

  if (opt_fastaout_discarded_rev)
    {
      fclose(fp_fastaout_discarded_rev);
    }

  fasta_close(h);
}
