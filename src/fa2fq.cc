/*

  VSEARCH: a versatile open source tool for metagenomics

  Copyright (C) 2014-2021, Torbjorn Rognes, Frederic Mahe and Tomas Flouri
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

void fasta2fastq()
{
  if (! opt_fastqout)
    fatal("Output FASTQ file not specified with the --fastqout option");

  fastx_handle h = fasta_open(opt_fasta2fastq);

  if (!h)
    fatal("Unable to open FASTA file for reading");

  FILE * fp_fastqout = nullptr;

  fp_fastqout = fopen_output(opt_fastqout);
  if (!fp_fastqout)
    fatal("Unable to open FASTQ output file for writing");

  const auto max_ascii_value = opt_fastq_asciiout + opt_fastq_qmaxout;
  int count = 0;
  size_t alloc = 0;
  char * quality = nullptr;

  progress_init("Converting FASTA file to FASTQ", fasta_get_size(h));

  while(fasta_next(h, 0, chrmap_no_change))
    {
      /* header */

      char * header = fasta_get_header(h);
      int hlen = fasta_get_header_length(h);
      int64_t abundance = fastq_get_abundance(h);

      /* sequence */

      uint64_t length = fastq_get_sequence_length(h);
      char * sequence = fastq_get_sequence(h);

      /* allocate more mem if necessary */

      if (alloc < length + 1)
        {
          alloc = length + 1;
          quality = (char*) xrealloc(quality, alloc);
        }

      /* set quality values */

      for(uint64_t i=0; i < length; i++)
        {
          quality[i] = max_ascii_value;
        }

      quality[length] = 0;

      count++;

      /* write to fasta file */

      fastq_print_general(fp_fastqout,
                          sequence,
                          length,
                          header,
                          hlen,
                          quality,
                          abundance,
                          count,
                          -1.0);

      progress_update(fasta_get_position(h));
    }

  progress_done();

  if (quality)
    {
      xfree(quality);
      quality = nullptr;
    }

  fclose(fp_fastqout);

  fasta_close(h);
}
