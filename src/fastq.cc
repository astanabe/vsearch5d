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

void fastq_fatal(uint64_t lineno, const char * msg)
{
  char * string;
  if (xsprintf(& string,
               "Invalid line %lu in FASTQ file: %s",
               lineno,
               msg) == -1)
    {
      fatal("Out of memory");
    }

  if (string)
    {
      fatal(string);
      xfree(string);
    }
  else
    {
      fatal("Out of memory");
    }
}

void buffer_filter_extend(fastx_handle h,
                          struct fastx_buffer_s * dest_buffer,
                          char * source_buf,
                          uint64_t len,
                          unsigned int * char_action,
                          const unsigned char * char_mapping,
                          bool * ok,
                          char * illegal_char)
{
  buffer_makespace(dest_buffer, len+1);

  /* Strip unwanted characters from the string and raise warnings or
     errors on certain characters. */

  char * p = source_buf;
  char * d = dest_buffer->data + dest_buffer->length;
  char * q = d;
  * ok = true;

  for(uint64_t i = 0; i < len; i++)
    {
      char c = *p++;
      char m = char_action[(unsigned char)c];

      switch(m)
        {
        case 0:
          /* stripped */
          h->stripped_all++;
          h->stripped[(unsigned char)c]++;
          break;

        case 1:
          /* legal character */
          *q++ = char_mapping[(unsigned char)(c)];
          break;

        case 2:
          /* fatal character */
          if (*ok)
            {
              * illegal_char = c;
            }
          * ok = false;
          break;

        case 3:
          /* silently stripped chars (whitespace) */
          break;

        case 4:
          /* newline (silently stripped) */
          break;
        }
    }

  /* add zero after sequence */
  *q = 0;
  dest_buffer->length += q - d;
}

fastx_handle fastq_open(const char * filename)
{
  fastx_handle h = fastx_open(filename);

  if (!fastx_is_fastq(h))
    {
      fatal("FASTQ file expected, FASTA file found (%s)", filename);
    }

  return h;
}

void fastq_close(fastx_handle h)
{
  fastx_close(h);
}

bool fastq_next(fastx_handle h,
                bool truncateatspace,
                const unsigned char * char_mapping)
{
  h->header_buffer.length = 0;
  h->header_buffer.data[0] = 0;
  h->sequence_buffer.length = 0;
  h->sequence_buffer.data[0] = 0;
  h->plusline_buffer.length = 0;
  h->plusline_buffer.data[0] = 0;
  h->quality_buffer.length = 0;
  h->quality_buffer.data[0] = 0;

  h->lineno_start = h->lineno;

  char msg[200];
  bool ok = true;
  char illegal_char = 0;

  uint64_t rest = fastx_file_fill_buffer(h);

  /* check end of file */

  if (rest == 0)
    {
      return false;
    }

  /* read header */

  /* check initial @ character */

  if (h->file_buffer.data[h->file_buffer.position] != '@')
    {
      fastq_fatal(h->lineno, "Header line must start with '@' character");
    }
  h->file_buffer.position++;
  rest--;

  char * lf = nullptr;
  while (lf == nullptr)
    {
      /* get more data if buffer empty */
      rest = fastx_file_fill_buffer(h);
      if (rest == 0)
        {
          fastq_fatal(h->lineno, "Unexpected end of file");
        }

      /* find LF */
      lf = (char *) memchr(h->file_buffer.data + h->file_buffer.position,
                           '\n',
                           rest);

      /* copy to header buffer */
      uint64_t len = rest;
      if (lf)
        {
          /* LF found, copy up to and including LF */
          len = lf - (h->file_buffer.data + h->file_buffer.position) + 1;
          h->lineno++;
        }
      buffer_extend(& h->header_buffer,
                    h->file_buffer.data + h->file_buffer.position,
                    len);
      h->file_buffer.position += len;
      rest -= len;
    }

  /* read sequence line(s) */
  lf = nullptr;
  while (true)
    {
      /* get more data, if necessary */
      rest = fastx_file_fill_buffer(h);

      /* cannot end here */
      if (rest == 0)
        {
          fastq_fatal(h->lineno, "Unexpected end of file");
        }

      /* end when new line starting with + is seen */
      if (lf && (h->file_buffer.data[h->file_buffer.position] == '+'))
        {
          break;
        }

      /* find LF */
      lf = (char *) memchr(h->file_buffer.data + h->file_buffer.position,
                           '\n', rest);

      /* copy to sequence buffer */
      uint64_t len = rest;
      if (lf)
        {
          /* LF found, copy up to and including LF */
          len = lf - (h->file_buffer.data + h->file_buffer.position) + 1;
          h->lineno++;
        }

      buffer_filter_extend(h,
                           & h->sequence_buffer,
                           h->file_buffer.data + h->file_buffer.position,
                           len,
                           char_fq_action_seq, char_mapping,
                           & ok, & illegal_char);
      h->file_buffer.position += len;
      rest -= len;

      if (!ok)
        {
          if ((illegal_char >= 32) && (illegal_char < 127))
            {
              snprintf(msg,
                       200,
                       "Illegal sequence character '%c'",
                       illegal_char);
            }
          else
            {
              snprintf(msg,
                       200,
                       "Illegal sequence character (unprintable, no %d)",
                       (unsigned char) illegal_char);
            }
          fastq_fatal(h->lineno - (lf ? 1 : 0), msg);
        }
    }

  /* read + line */

  /* skip + character */
  h->file_buffer.position++;
  rest--;

  lf = nullptr;
  while (lf == nullptr)
    {
      /* get more data if buffer empty */
      rest = fastx_file_fill_buffer(h);

      /* cannot end here */
      if (rest == 0)
        {
          fastq_fatal(h->lineno, "Unexpected end of file");
        }

      /* find LF */
      lf = (char *) memchr(h->file_buffer.data + h->file_buffer.position,
                           '\n',
                           rest);
      /* copy to plusline buffer */
      uint64_t len = rest;
      if (lf)
        {
          /* LF found, copy up to and including LF */
          len = lf - (h->file_buffer.data + h->file_buffer.position) + 1;
          h->lineno++;
        }
      buffer_extend(& h->plusline_buffer,
                    h->file_buffer.data + h->file_buffer.position,
                    len);
      h->file_buffer.position += len;
      rest -= len;
    }

  /* check that the plus line is empty or identical to @ line */

  bool plusline_invalid = false;
  if (h->header_buffer.length == h->plusline_buffer.length)
    {
      if (memcmp(h->header_buffer.data,
                 h->plusline_buffer.data,
                 h->header_buffer.length))
        {
          plusline_invalid = true;
        }
    }
  else
    {
      if ((h->plusline_buffer.length > 2) ||
          ((h->plusline_buffer.length == 2) && (h->plusline_buffer.data[0] != '\r')))
        {
          plusline_invalid = true;
        }
    }
  if (plusline_invalid)
    {
      fastq_fatal(h->lineno - (lf ? 1 : 0),
                  "'+' line must be empty or identical to header");
    }

  /* read quality line(s) */

  lf = nullptr;
  while (true)
    {
      /* get more data, if necessary */
      rest = fastx_file_fill_buffer(h);

      /* end if no more data */
      if (rest == 0)
        {
          break;
        }

      /* end if next entry starts : LF + '@' + correct length */
      if (lf &&
          (h->file_buffer.data[h->file_buffer.position] == '@') &&
          (h->quality_buffer.length == h->sequence_buffer.length))
        {
          break;
        }

      /* find LF */
      lf = (char *) memchr(h->file_buffer.data + h->file_buffer.position,
                           '\n', rest);

      /* copy to quality buffer */
      uint64_t len = rest;
      if (lf)
        {
          /* LF found, copy up to and including LF */
          len = lf - (h->file_buffer.data + h->file_buffer.position) + 1;
          h->lineno++;
        }

      buffer_filter_extend(h,
                           & h->quality_buffer,
                           h->file_buffer.data + h->file_buffer.position,
                           len,
                           char_fq_action_qual, chrmap_identity,
                           & ok, & illegal_char);
      h->file_buffer.position += len;
      rest -= len;

      /* break if quality line already too long */
      if (h->quality_buffer.length > h->sequence_buffer.length)
        {
          break;
        }

      if (!ok)
        {
          if ((illegal_char >= 32) && (illegal_char < 127))
            {
              snprintf(msg,
                       200,
                       "Illegal quality character '%c'",
                       illegal_char);
            }
          else
            {
              snprintf(msg,
                       200,
                       "Illegal quality character (unprintable, no %d)",
                       (unsigned char) illegal_char);
            }
          fastq_fatal(h->lineno - (lf ? 1 : 0), msg);
        }
    }

  if (h->sequence_buffer.length != h->quality_buffer.length)
    {
      fastq_fatal(h->lineno - (lf ? 1 : 0),
                  "Sequence and quality lines must be equally long");
    }

  fastx_filter_header(h, truncateatspace);

  h->seqno++;

  return true;
}

char * fastq_get_quality(fastx_handle h)
{
  return h->quality_buffer.data;
}

uint64_t fastq_get_quality_length(fastx_handle h)
{
  return h->quality_buffer.length;
}

uint64_t fastq_get_position(fastx_handle h)
{
  return h->file_position;
}

uint64_t fastq_get_size(fastx_handle h)
{
  return h->file_size;
}

uint64_t fastq_get_lineno(fastx_handle h)
{
  return h->lineno_start;
}

uint64_t fastq_get_seqno(fastx_handle h)
{
  return h->seqno;
}

uint64_t fastq_get_header_length(fastx_handle h)
{
  return h->header_buffer.length;
}

uint64_t fastq_get_sequence_length(fastx_handle h)
{
  return h->sequence_buffer.length;
}

char * fastq_get_header(fastx_handle h)
{
  return h->header_buffer.data;
}

char * fastq_get_sequence(fastx_handle h)
{
  return h->sequence_buffer.data;
}

int64_t fastq_get_abundance(fastx_handle h)
{
  // return 1 if not present
  int64_t size = header_get_size(h->header_buffer.data,
                                 h->header_buffer.length);
  if (size > 0)
    {
      return size;
    }
  else
    {
      return 1;
    }
}

int64_t fastq_get_abundance_and_presence(fastx_handle h)
{
  // return 0 if not present
  return header_get_size(h->header_buffer.data, h->header_buffer.length);
}

inline void fprint_seq_label(FILE * fp, char * seq, int len)
{
  /* normalize first? */
  fprintf(fp, "%.*s", len, seq);
}

void fastq_print_general(FILE * fp,
                         char * seq,
                         int len,
                         char * header,
                         int header_len,
                         char * quality,
                         int abundance,
                         int ordinal,
                         double ee)
{
  fprintf(fp, "@");

  if (opt_relabel_self)
    {
      fprint_seq_label(fp, seq, len);
    }
  else if (opt_relabel_sha1)
    {
      fprint_seq_digest_sha1(fp, seq, len);
    }
  else if (opt_relabel_md5)
    {
      fprint_seq_digest_md5(fp, seq, len);
    }
  else if (opt_relabel && (ordinal > 0))
    {
      fprintf(fp, "%s%d", opt_relabel, ordinal);
    }
  else
    {
      bool xsize = opt_xsize || (opt_sizeout && (abundance > 0));
      bool xee = opt_xee || ((opt_eeout || opt_fastq_eeout) && (ee >= 0.0));
      header_fprint_strip_size_ee(fp,
                                  header,
                                  header_len,
                                  xsize,
                                  xee);
    }

  if (opt_label_suffix)
    {
      fprintf(fp, "%s", opt_label_suffix);
    }

  if (opt_sample)
    {
      fprintf(fp, ";sample=%s", opt_sample);
    }

  if (opt_sizeout && (abundance > 0))
    {
      fprintf(fp, ";size=%u", abundance);
    }

  if ((opt_eeout || opt_fastq_eeout) && (ee >= 0.0))
    {
      fprintf(fp, ";ee=%.4lf", ee);
    }

  if (opt_relabel_keep &&
      ((opt_relabel && (ordinal > 0)) || opt_relabel_sha1 || opt_relabel_md5 || opt_relabel_self))
    {
      fprintf(fp, " %.*s", header_len, header);
    }

  fprintf(fp, "\n%.*s\n+\n%.*s\n", len, seq, len, quality);
}

void fastq_print(FILE * fp, char * header, char * sequence, char * quality)
{
  int slen = strlen(sequence);
  int hlen = strlen(header);
  fastq_print_general(fp, sequence, slen, header, hlen, quality, 0, 0, -1.0);
}
