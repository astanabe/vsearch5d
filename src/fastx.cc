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
#include "dynlibs.h"
#include "utils/fatal.hpp"
#include "utils/span.hpp"
#include <unistd.h>  // dup, STDIN_FILENO, STDOUT_FILENO
#include <algorithm>  // std::find_first_of
#include <array>
#include <cinttypes>  // macros PRIu64 and PRId64
#include <cstdint>  // int64_t, uint64_t
#include <cstdio>  // std::FILE, std::fprintf, std::fclose, std::fopen, std::size_t, std::fread, std::fileno
#include <cstdlib>  // std::exit, EXIT_FAILURE
#include <cstring>  // std::memcpy, std::memcmp, std::strcmp
#include <iterator> // std::distance
#include <vector>


/* file compression and format detector */
/* basic file buffering function for fastq and fastx parsers */

constexpr uint64_t fastx_buffer_alloc = 8192;

#ifdef HAVE_BZLIB_H
constexpr auto BZ_VERBOSE_0 = 0;
// constexpr auto BZ_VERBOSE_1 = 1;
// constexpr auto BZ_VERBOSE_2 = 2;
// constexpr auto BZ_VERBOSE_3 = 3;
// constexpr auto BZ_VERBOSE_4 = 4;
constexpr auto BZ_MORE_MEM = 0;  /* faster decompression using more memory */
// constexpr auto BZ_LESS_MEM = 1;  /* slower decompression but requires less memory */
#endif

constexpr std::array<unsigned char, 2> magic_gzip = {0x1f, 0x8b};
constexpr std::array<unsigned char, 2> magic_bzip = {'B', 'Z'};


auto buffer_init(struct fastx_buffer_s * buffer) -> void
{
  buffer->alloc = fastx_buffer_alloc;
  buffer->data = static_cast<char *>(xmalloc(buffer->alloc));
  buffer->data[0] = 0;
  buffer->length = 0;
  buffer->position = 0;
}


auto buffer_free(struct fastx_buffer_s * buffer) -> void
{
  if (buffer->data != nullptr)
    {
      xfree(buffer->data);
    }
  buffer->data = nullptr;
  buffer->alloc = 0;
  buffer->length = 0;
  buffer->position = 0;
}


auto buffer_makespace(struct fastx_buffer_s * buffer, uint64_t size) -> void
{
  /* make sure there is space for x more chars in buffer */

  if (buffer->length + size > buffer->alloc)
    {
      /* alloc space for x more characters,
         but round up to nearest block size */
      buffer->alloc =
        ((buffer->length + size + fastx_buffer_alloc - 1) / fastx_buffer_alloc)
        * fastx_buffer_alloc;
      buffer->data = (char *) xrealloc(buffer->data, buffer->alloc);
    }
}


auto buffer_extend(struct fastx_buffer_s * dest_buffer,
                   char * source_buf,
                   uint64_t len) -> void
{
  buffer_makespace(dest_buffer, len + 1);
  std::memcpy(dest_buffer->data + dest_buffer->length,
         source_buf,
         len);
  dest_buffer->length += len;
  dest_buffer->data[dest_buffer->length] = 0;
}


auto find_header_end_first_blank(Span<char> raw_header) -> std::size_t {
  static const std::vector<char> blanks {' ', '\t', '\0', '\r', '\n'};
  auto * result = std::find_first_of(raw_header.begin(), raw_header.end(),
                                     blanks.begin(), blanks.end());
  if (result != raw_header.end()) {
    *result = '\0';
  }
  return std::distance(raw_header.begin(), result);
}


auto find_header_end(Span<char> raw_header) -> std::size_t {
  static const std::vector<char> blanks {'\0', '\r', '\n'};
  auto * result = std::find_first_of(raw_header.begin(), raw_header.end(),
                                     blanks.begin(), blanks.end());
  if (result != raw_header.end()) {
    *result = '\0';
  }
  return std::distance(raw_header.begin(), result);
}


auto warn(char const * const format,
          unsigned char const symbol,
          uint64_t const line_number) -> void {
  std::fprintf(stderr, "\nWARNING: ");
  std::fprintf(stderr, format, symbol, symbol, line_number);
  std::fprintf(stderr, "\n");

  if (fp_log != nullptr) {
    std::fprintf(fp_log, "\nWARNING: ");
    std::fprintf(fp_log, format, symbol, symbol, line_number);
    std::fprintf(fp_log, "\n");
  }
}


auto fastx_filter_header(fastx_handle input_handle, bool truncateatspace) -> void {
  // truncate header (in-place)
  auto raw_header = Span<char>{input_handle->header_buffer.data, input_handle->header_buffer.length};
  auto const count = truncateatspace ? find_header_end_first_blank(raw_header) : find_header_end(raw_header);
  input_handle->header_buffer.length = count;

  // scan for unusual symbols
  auto const trimmed_header = raw_header.first(count);
  for (auto const symbol: trimmed_header) {
    auto const is_illegal = ((symbol == 127) or
                             ((symbol > '\0') and (symbol < ' ') and not (symbol == '\t')));
    if (is_illegal) {
      fatal("Illegal character encountered in FASTA/FASTQ header.\n"
            "Unprintable ASCII character no %d on line %" PRIu64 ".",
            symbol, input_handle->lineno_start);
    }
    auto const symbol_unsigned = static_cast<unsigned char>(symbol);
    auto const is_not_ascii = (symbol_unsigned > 127);
    if (is_not_ascii) {
      warn("Non-ASCII character encountered in FASTA/FASTQ header.\n"
           "Character no %d (0x%2x) on line %" PRIu64 ".",
           symbol_unsigned,
           input_handle->lineno_start);
    }
  }
}


auto fopen_input(const char * filename) -> std::FILE *
{
  /* open the input stream given by filename, but use stdin if name is - */
  if (std::strcmp(filename, "-") == 0)
    {
      auto const file_descriptor = dup(STDIN_FILENO);
      if (file_descriptor < 0)
        {
          return nullptr;
        }
      return fdopen(file_descriptor, "rb");
    }
  return std::fopen(filename, "rb");
}


auto fastx_open(char const * filename) -> fastx_handle
{
  // refactoring: duplicate function to output a struct fastx_s input_handle_s;
  auto * input_handle = (fastx_handle) xmalloc(sizeof(struct fastx_s));

  input_handle->fp = nullptr;

#ifdef HAVE_ZLIB_H
  input_handle->fp_gz = nullptr;
#endif

#ifdef HAVE_BZLIB_H
  input_handle->fp_bz = nullptr;
  int bzError = 0;
#endif

  input_handle->fp = fopen_input(filename);
  if (input_handle->fp == nullptr)
    {
      fatal("Unable to open file for reading (%s)", filename);
    }

  /* Get mode and size of original (uncompressed) file */

  xstat_t fs;
  if (xfstat(fileno(input_handle->fp), & fs) != 0)
    {
      fatal("Unable to get status for input file (%s)", filename);
    }

  input_handle->is_pipe = S_ISFIFO(fs.st_mode);

  if (input_handle->is_pipe)
    {
      input_handle->file_size = 0;
    }
  else
    {
      input_handle->file_size = fs.st_size;
    }

  if (opt_gzip_decompress)
    {
      input_handle->format = Format::gzip;
    }
  else if (opt_bzip2_decompress)
    {
      input_handle->format = Format::bzip;
    }
  else if (input_handle->is_pipe)
    {
      input_handle->format = Format::plain;
    }
  else
    {
      /* autodetect compression (plain, gzipped or bzipped) */

      /* read two characters and compare with magic */

      std::array<unsigned char, 2> magic {{}};

      input_handle->format = Format::plain;

      auto const bytes_read = fread(magic.data(), 1, 2, input_handle->fp);

      if (bytes_read >= 2)
        {
          if (memcmp(magic.data(), magic_gzip.data(), 2) == 0)
            {
              input_handle->format = Format::gzip;
            }
          else if (memcmp(magic.data(), magic_bzip.data(), 2) == 0)
            {
              input_handle->format = Format::bzip;
            }
        }
      else
        {
          /* consider it an empty file or a tiny fasta file, uncompressed */
        }

      /* close and reopen to avoid problems with gzip library */
      /* rewind was not enough */

      fclose(input_handle->fp);
      input_handle->fp = fopen_input(filename);
      if (input_handle->fp == nullptr)
        {
          fatal("Unable to open file for reading (%s)", filename);
        }
    }

  if (input_handle->format == Format::gzip)
    {
      /* GZIP: Keep original file open, then open as gzipped file as well */
#ifdef HAVE_ZLIB_H
      if (gz_lib == nullptr)
        {
          fatal("Files compressed with gzip are not supported");
        }
      input_handle->fp_gz = (*gzdopen_p)(fileno(input_handle->fp), "rb");
      if (input_handle->fp_gz == nullptr)
        { // dup?
          fatal("Unable to open gzip compressed file (%s)", filename);
        }
#else
      fatal("Files compressed with gzip are not supported");
#endif
    }

  if (input_handle->format == Format::bzip)
    {
      /* BZIP2: Keep original file open, then open as bzipped file as well */
#ifdef HAVE_BZLIB_H
      if (bz2_lib == nullptr)
        {
          fatal("Files compressed with bzip2 are not supported");
        }
      input_handle->fp_bz = (*BZ2_bzReadOpen_p)(& bzError, input_handle->fp,
                                     BZ_VERBOSE_0, BZ_MORE_MEM,
                                     nullptr, 0);
      if (input_handle->fp_bz == nullptr)
        {
          fatal("Unable to open bzip2 compressed file (%s)", filename);
        }
#else
      fatal("Files compressed with bzip2 are not supported");
#endif
    }

  /* init buffers */

  input_handle->file_position = 0;

  buffer_init(& input_handle->file_buffer);

  /* start filling up file buffer */

  auto const rest = fastx_file_fill_buffer(input_handle);

  /* examine first char and see if it starts with > or @ */

  auto filetype = 0;
  input_handle->is_empty = true;
  input_handle->is_fastq = false;

  if (rest > 0)
    {
      input_handle->is_empty = false;

      auto * first = input_handle->file_buffer.data;

      if (*first == '>')
        {
          filetype = 1;
        }
      else if (*first == '@')
        {
          filetype = 2;
          input_handle->is_fastq = true;
        }

      if (filetype == 0)
        {
          /* close files if unrecognized file type */

          switch(input_handle->format)
            {
            case Format::plain:
              break;

            case Format::gzip:
#ifdef HAVE_ZLIB_H
              (*gzclose_p)(input_handle->fp_gz);
              input_handle->fp_gz = nullptr;
              break;
#endif

            case Format::bzip:
#ifdef HAVE_BZLIB_H
              (*BZ2_bzReadClose_p)(&bzError, input_handle->fp_bz);
              input_handle->fp_bz = nullptr;
              break;
#endif

            default:
              fatal("Internal error");
            }

          fclose(input_handle->fp);
          input_handle->fp = nullptr;

          if (rest >= 2)
            {
              if (memcmp(first, magic_gzip.data(), 2) == 0)
                {
                  fatal("File appears to be gzip compressed. Please use --gzip_decompress");
                }

              if (memcmp(first, magic_bzip.data(), 2) == 0)
                {
                  fatal("File appears to be bzip2 compressed. Please use --bzip2_decompress");
                }
            }

          fatal("File type not recognized.");

          return nullptr;
        }
    }

  /* more initialization */

  buffer_init(& input_handle->header_buffer);
  buffer_init(& input_handle->sequence_buffer);
  buffer_init(& input_handle->plusline_buffer);
  buffer_init(& input_handle->quality_buffer);

  input_handle->stripped_all = 0;

  input_handle->lineno = 1;
  input_handle->lineno_start = 1;
  input_handle->seqno = -1;

  return input_handle;
}


auto fastx_is_fastq(fastx_handle input_handle) -> bool
{
  return input_handle->is_fastq or input_handle->is_empty;
}


auto fastx_is_empty(fastx_handle input_handle) -> bool
{
  return input_handle->is_empty;
}


auto fastx_is_pipe(fastx_handle input_handle) -> bool
{
  return input_handle->is_pipe;
}


auto fastx_close(fastx_handle input_handle) -> void
{
  /* Warn about stripped chars */

  if (input_handle->stripped_all != 0U)
    {
      fprintf(stderr, "WARNING: %" PRIu64 " invalid characters stripped from %s file:", input_handle->stripped_all, (input_handle->is_fastq ? "FASTQ" : "FASTA"));
      for (int i = 0; i < 256; i++)
        {
          if (input_handle->stripped[i] != 0U)
            {
              fprintf(stderr, " %c(%" PRIu64 ")", i, input_handle->stripped[i]);
            }
        }
      fprintf(stderr, "\n");
      fprintf(stderr, "REMINDER: vsearch does not support amino acid sequences\n");

      if (opt_log != nullptr)
        {
          fprintf(fp_log, "WARNING: %" PRIu64 " invalid characters stripped from %s file:", input_handle->stripped_all, (input_handle->is_fastq ? "FASTQ" : "FASTA"));
          for (int i = 0; i < 256; i++)
            {
              if (input_handle->stripped[i] != 0U)
                {
                  fprintf(fp_log, " %c(%" PRIu64 ")", i, input_handle->stripped[i]);
                }
            }
          fprintf(fp_log, "\n");
          fprintf(fp_log, "REMINDER: vsearch does not support amino acid sequences\n");
        }
    }

#ifdef HAVE_BZLIB_H
  int bz_error = 0;
#endif

  switch(input_handle->format)
    {
    case Format::plain:
      break;

    case Format::gzip:
#ifdef HAVE_ZLIB_H
      (*gzclose_p)(input_handle->fp_gz);
      input_handle->fp_gz = nullptr;
      break;
#endif

    case Format::bzip:
#ifdef HAVE_BZLIB_H
      (*BZ2_bzReadClose_p)(&bz_error, input_handle->fp_bz);
      input_handle->fp_bz = nullptr;
      break;
#endif

    default:
      fatal("Internal error");
    }

  fclose(input_handle->fp);
  input_handle->fp = nullptr;

  buffer_free(& input_handle->file_buffer);
  buffer_free(& input_handle->header_buffer);
  buffer_free(& input_handle->sequence_buffer);
  buffer_free(& input_handle->plusline_buffer);
  buffer_free(& input_handle->quality_buffer);

  input_handle->file_size = 0;
  input_handle->file_position = 0;

  input_handle->lineno = 0;
  input_handle->seqno = -1;

  xfree(input_handle);
  input_handle = nullptr;
}


auto fastx_file_fill_buffer(fastx_handle input_handle) -> uint64_t
{
  /* read more data if necessary */
  uint64_t const rest = input_handle->file_buffer.length - input_handle->file_buffer.position;

  if (rest > 0)
    {
      return rest;
    }

  uint64_t space = input_handle->file_buffer.alloc - input_handle->file_buffer.length;

  if (space == 0)
    {
      /* back to beginning of buffer */
      input_handle->file_buffer.position = 0;
      input_handle->file_buffer.length = 0;
      space = input_handle->file_buffer.alloc;
    }

  int bytes_read = 0;

#ifdef HAVE_BZLIB_H
  int bzError = 0;
#endif

  switch(input_handle->format)
    {
    case Format::plain:
      bytes_read = fread(input_handle->file_buffer.data
                         + input_handle->file_buffer.position,
                         1,
                         space,
                         input_handle->fp);
      break;

    case Format::gzip:
#ifdef HAVE_ZLIB_H
      bytes_read = (*gzread_p)(input_handle->fp_gz,
                               input_handle->file_buffer.data
                               + input_handle->file_buffer.position,
                               space);
      if (bytes_read < 0)
        {
          fatal("Unable to read gzip compressed file");
        }
      break;
#endif

    case Format::bzip:
#ifdef HAVE_BZLIB_H
      bytes_read = (*BZ2_bzRead_p)(& bzError,
                                   input_handle->fp_bz,
                                   input_handle->file_buffer.data
                                   + input_handle->file_buffer.position,
                                   space);
      if ((bytes_read < 0) or
          not ((bzError == BZ_OK) or
             (bzError == BZ_STREAM_END) or
             (bzError == BZ_SEQUENCE_ERROR)))
        {
          fatal("Unable to read from bzip2 compressed file");
        }
      break;
#endif

    default:
      fatal("Internal error");
    }

  if (not input_handle->is_pipe)
    {
#ifdef HAVE_ZLIB_H
      if (input_handle->format == Format::gzip)
        {
          /* Circumvent the missing gzoffset function in zlib 1.2.3 and earlier */
          int const fd = dup(fileno(input_handle->fp));
          input_handle->file_position = xlseek(fd, 0, SEEK_CUR);
          close(fd);
        }
      else
#endif
        {
          input_handle->file_position = xftello(input_handle->fp);
        }
    }

  input_handle->file_buffer.length += bytes_read;
  return bytes_read;
}


auto fastx_next(fastx_handle input_handle,
                bool truncateatspace,
                const unsigned char * char_mapping) -> bool
{
  if (input_handle->is_fastq)
    {
      return fastq_next(input_handle, truncateatspace, char_mapping);
    }
  return fasta_next(input_handle, truncateatspace, char_mapping);
}


auto fastx_get_position(fastx_handle input_handle) -> uint64_t
{
  if (input_handle->is_fastq)
    {
      return fastq_get_position(input_handle);
    }
  return fasta_get_position(input_handle);
}


auto fastx_get_size(fastx_handle input_handle) -> uint64_t
{
  if (input_handle->is_fastq)
    {
      return fastq_get_size(input_handle);
    }
  return fasta_get_size(input_handle);
}


auto fastx_get_lineno(fastx_handle input_handle) -> uint64_t
{
  if (input_handle->is_fastq)
    {
      return fastq_get_lineno(input_handle);
    }
  return fasta_get_lineno(input_handle);
}


auto fastx_get_seqno(fastx_handle input_handle) -> uint64_t
{
  if (input_handle->is_fastq)
    {
      return fastq_get_seqno(input_handle);
    }
  return fasta_get_seqno(input_handle);
}


auto fastx_get_header(fastx_handle input_handle) -> char *
{
  if (input_handle->is_fastq)
    {
      return fastq_get_header(input_handle);
    }
  return fasta_get_header(input_handle);
}


auto fastx_get_sequence(fastx_handle input_handle) -> char *
{
  if (input_handle->is_fastq)
    {
      return fastq_get_sequence(input_handle);
    }
  return fasta_get_sequence(input_handle);
}


auto fastx_get_header_length(fastx_handle input_handle) -> uint64_t
{
  if (input_handle->is_fastq)
    {
      return fastq_get_header_length(input_handle);
    }
  return fasta_get_header_length(input_handle);
}


auto fastx_get_sequence_length(fastx_handle input_handle) -> uint64_t
{
  if (input_handle->is_fastq)
    {
      return fastq_get_sequence_length(input_handle);
    }
  return fasta_get_sequence_length(input_handle);
}


auto fastx_get_quality(fastx_handle input_handle) -> char *
{
  if (input_handle->is_fastq)
    {
      return fastq_get_quality(input_handle);
    }
  return nullptr;
}


auto fastx_get_abundance(fastx_handle input_handle) -> int64_t
{
  if (input_handle->is_fastq)
    {
      return fastq_get_abundance(input_handle);
    }
  return fasta_get_abundance(input_handle);
}
