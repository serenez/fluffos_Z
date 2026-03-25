/* Compression efun stuff.
 * Started Wed Mar 21 01:52:25 PST 2001
 * by David Bennett (ddt@discworld.imaginary.com)
 */

#include "base/package_api.h"

#include "packages/core/file.h"

#include <string>
#include <zlib.h>

#define GZ_EXTENSION ".gz"

enum { COMPRESS_BUF_SIZE = 8096 };

namespace {

bool has_gz_extension(const char *path) {
  const auto path_len = strlen(path);
  const auto ext_len = strlen(GZ_EXTENSION);
  return path_len >= ext_len && strcmp(path + path_len - ext_len, GZ_EXTENSION) == 0;
}

}  // namespace

#ifdef F_COMPRESS_FILE
void f_compress_file() {
  int readb;
  int const num_arg = st_num_arg;
  const char *input_file;
  const char *output_file;
  const char *real_input_file;
  const char *real_output_file;
  gzFile out_file;
  FILE *in_file;
  char buf[4096];
  std::string output_path;

  // Not a string?  Error!
  if ((sp - num_arg + 1)->type != T_STRING) {
    pop_n_elems(num_arg);
    push_number(0);
    return;
  }

  input_file = (sp - num_arg + 1)->u.string;
  if (num_arg == 2) {
    if (((sp - num_arg + 2)->type != T_STRING)) {
      pop_n_elems(num_arg);
      push_number(0);
      return;
    }
    output_file = (sp - num_arg + 2)->u.string;
  } else {
    if (has_gz_extension(input_file)) {
      // Already compressed...
      pop_n_elems(num_arg);
      push_number(0);
      return;
    }
    output_path = input_file;
    output_path += GZ_EXTENSION;
    output_file = output_path.c_str();
  }

  real_output_file = check_valid_path(output_file, current_object, "compress_file", 1);
  if (!real_output_file) {
    pop_n_elems(num_arg);
    push_number(0);
    return;
  }
  output_path = real_output_file;
  output_file = output_path.c_str();

  real_input_file = check_valid_path(input_file, current_object, "compress_file", 0);
  if (!real_input_file) {
    pop_n_elems(num_arg);
    push_number(0);
    return;
  }

  in_file = fopen(real_input_file, "rb");
  if (!in_file) {
    pop_n_elems(num_arg);
    push_number(0);
    return;
  }

  out_file = gzopen(output_file, "wb");
  if (!out_file) {
    fclose(in_file);
    pop_n_elems(num_arg);
    push_number(0);
    return;
  }

  do {
    readb = fread(buf, 1, 4096, in_file);
    gzwrite(out_file, buf, readb);
  } while (readb == 4096);
  fclose(in_file);
  gzclose(out_file);

  unlink(real_input_file);

  pop_n_elems(num_arg);
  push_number(1);
}
#endif

#ifdef F_UNCOMPRESS_FILE
void f_uncompress_file() {
  int readb;
  int const num_arg = st_num_arg;
  const char *input_file;
  const char *output_file;
  const char *real_input_file;
  const char *real_output_file;
  FILE *out_file;
  gzFile in_file;
  char buf[4196];
  std::string output_path;

  // Not a string?  Error!
  if ((sp - num_arg + 1)->type != T_STRING) {
    pop_n_elems(num_arg);
    push_number(0);
    return;
  }

  input_file = (sp - num_arg + 1)->u.string;
  if (num_arg == 2) {
    if (((sp - num_arg + 2)->type != T_STRING)) {
      pop_n_elems(num_arg);
      push_number(0);
      return;
    }
    output_file = (sp - num_arg + 2)->u.string;
  } else {
    const auto len = strlen(input_file);
    if (!has_gz_extension(input_file)) {
      // Not compressed...
      pop_n_elems(num_arg);
      push_number(0);
      return;
    }
    output_path.assign(input_file, len - strlen(GZ_EXTENSION));
    output_file = output_path.c_str();
  }

  real_output_file = check_valid_path(output_file, current_object, "compress_file", 1);
  if (!real_output_file) {
    pop_n_elems(num_arg);
    push_number(0);
    return;
  }
  output_path = real_output_file;
  output_file = output_path.c_str();

  real_input_file = check_valid_path(input_file, current_object, "compress_file", 0);
  if (!real_input_file) {
    pop_n_elems(num_arg);
    push_number(0);
    return;
  }

  in_file = gzopen(real_input_file, "rb");
  if (!in_file) {
    pop_n_elems(num_arg);
    push_number(0);
    return;
  }

  out_file = fopen(output_file, "wb");
  if (!out_file) {
    gzclose(in_file);
    pop_n_elems(num_arg);
    push_number(0);
    return;
  }

  do {
    readb = gzread(in_file, buf, 4096);
    fwrite(buf, 1, readb, out_file);
  } while (readb == 4096);
  gzclose(in_file);
  fclose(out_file);

  unlink(real_input_file);

  pop_n_elems(num_arg);
  push_number(1);
}
#endif

#ifdef F_COMPRESS
void f_compress() {
  unsigned char *buffer;
  unsigned char *input;
  int size;
  buffer_t *real_buffer;
  uLongf new_size;

  if (sp->type == T_STRING) {
    size = SVALUE_STRLEN(sp);
    input = (unsigned char *)sp->u.string;
  } else if (sp->type == T_BUFFER) {
    size = sp->u.buf->size;
    input = sp->u.buf->item;
  } else {
    pop_n_elems(st_num_arg);
    push_undefined();
    return;
  }

  new_size = compressBound(size);
  // Make it a little larger as specified in the docs.
  buffer = reinterpret_cast<unsigned char *>(DMALLOC(new_size, TAG_TEMPORARY, "compress"));
  compress(buffer, &new_size, input, size);

  // Shrink it down.
  pop_n_elems(st_num_arg);
  real_buffer = allocate_buffer(new_size);
  write_buffer(real_buffer, 0, reinterpret_cast<char *>(buffer), new_size);
  FREE(buffer);
  push_refed_buffer(real_buffer);
}
#endif

#ifdef F_UNCOMPRESS
static void *zlib_alloc(void * /*opaque*/, unsigned int items, unsigned int size) {
  return DCALLOC(items, size, TAG_TEMPORARY, "zlib_alloc");
}

static void zlib_free(void * /*opaque*/, void *address) { FREE(address); }

void f_uncompress() {
  z_stream *compressed;
  unsigned char compress_buf[COMPRESS_BUF_SIZE];
  unsigned char *output_data = nullptr;
  int len;
  int pos;
  buffer_t *buffer;
  int ret;

  if (sp->type == T_BUFFER) {
    buffer = sp->u.buf;
  } else {
    pop_n_elems(st_num_arg);
    push_undefined();
    return;
  }

  compressed =
      reinterpret_cast<z_stream *>(DMALLOC(sizeof(z_stream), TAG_INTERACTIVE, "start_compression"));
  compressed->next_in = buffer->item;
  compressed->avail_in = buffer->size;
  compressed->next_out = compress_buf;
  compressed->avail_out = COMPRESS_BUF_SIZE;
  compressed->zalloc = zlib_alloc;
  compressed->zfree = zlib_free;
  compressed->opaque = nullptr;

  if (inflateInit(compressed) != Z_OK) {
    FREE(compressed);
    pop_n_elems(st_num_arg);
    error("inflateInit failed");
  }

  len = 0;
  output_data = nullptr;
  do {
    ret = inflate(compressed, 0);
    if (ret == Z_OK || ret == Z_STREAM_END) {
      pos = len;
      len += COMPRESS_BUF_SIZE - compressed->avail_out;
      if (!output_data) {
        output_data = reinterpret_cast<unsigned char *>(DMALLOC(len, TAG_TEMPORARY, "uncompress"));
      } else {
        output_data = reinterpret_cast<unsigned char *>(
            DREALLOC(output_data, len, TAG_TEMPORARY, "uncompress"));
      }
      memcpy(output_data + pos, compress_buf, len - pos);
      compressed->next_out = compress_buf;
      compressed->avail_out = COMPRESS_BUF_SIZE;
    }
  } while (ret == Z_OK);

  inflateEnd(compressed);

  pop_n_elems(st_num_arg);

  if (ret == Z_STREAM_END) {
    buffer = allocate_buffer(len);
    write_buffer(buffer, 0, reinterpret_cast<char *>(output_data), len);
    FREE(output_data);
    push_refed_buffer(buffer);
    FREE(compressed);
  } else {
    FREE(compressed);
    error("inflate: no ZSTREAM_END\n");
  }
}
#endif
