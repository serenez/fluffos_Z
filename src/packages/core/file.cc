/*
 * file: file.c
 * description: handle all file based efuns
 */
#include "base/package_api.h"

#include "base/internal/tracing.h"
#include "packages/core/file.h"

#include <algorithm>
#include <iostream>
#include <cerrno>
#if HAVE_DIRENT_H
#include <dirent.h>
#define NAMLEN(dirent) strlen((dirent)->d_name)
#else
#define dirent direct
#define NAMLEN(dirent) (dirent)->d_namlen
#if HAVE_SYS_NDIR_H
#include <sys/ndir.h>
#endif
#if HAVE_SYS_DIR_H
#include <sys/dir.h>
#endif
#if HAVE_NDIR_H
#include <ndir.h>
#endif
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif
#ifdef HAVE_SYS_SOCKIO_H
#include <sys/sockio.h>
#endif
#ifdef HAVE_SYS_MKDEV_H
#include <sys/mkdev.h>
#endif
#include <fcntl.h>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>
#include <zlib.h>

#include "base/internal/strutils.h"
#include "ghc/filesystem.hpp"
namespace fs = ghc::filesystem;

/*
 * Credits for some of the code below goes to Free Software Foundation
 * Copyright (C) 1990 Free Software Foundation, Inc.
 * See the GNU General Public License for more details.
 */
#ifndef S_ISDIR
#define S_ISDIR(m) (((m)&S_IFMT) == S_IFDIR)
#endif

#ifndef S_ISREG
#define S_ISREG(m) (((m)&S_IFMT) == S_IFREG)
#endif

#ifndef S_ISCHR
#define S_ISCHR(m) (((m)&S_IFMT) == S_IFCHR)
#endif

#ifndef S_ISBLK
#define S_ISBLK(m) (((m)&S_IFMT) == S_IFBLK)
#endif

#ifdef _WIN32
#define lstat(x, y) stat(x, y)
#define link(x, y) ((-1))
#define OS_mkdir(x, y) mkdir(x)
#else
#define OS_mkdir(x, y) mkdir(x, y)
#endif

static int match_string(const char * /*match*/, const char * /*str*/);
static int do_move(const char *from, const char *to, int flag);
static void encode_stat(svalue_t * /*vp*/, int /*flags*/, const char * /*str*/, struct stat * /*st*/);

static std::string append_path_component(const char *dir, const char *name) {
  std::string path(dir);
  if (!path.empty() && path.back() != '/') {
    path.push_back('/');
  }
  path += name;
  return path;
}

enum { MAX_LINES = 50 };

static void encode_stat(svalue_t *vp, int flags, const char *str, struct stat *st) {
  if (flags == -1) {
    array_t *v = allocate_empty_array(3);

    v->item[0].type = T_STRING;
    v->item[0].subtype = STRING_MALLOC;
    v->item[0].u.string = string_copy(str, "encode_stat");
    v->item[1].type = T_NUMBER;
    v->item[1].u.number = ((st->st_mode & S_IFDIR) ? -2 : st->st_size);
    v->item[2].type = T_NUMBER;
    v->item[2].u.number = st->st_mtime;
    vp->type = T_ARRAY;
    vp->u.arr = v;
  } else {
    vp->type = T_STRING;
    vp->subtype = STRING_MALLOC;
    vp->u.string = string_copy(str, "encode_stat");
  }
}

/*
 * List files in directory. This function do same as standard list_files did,
 * but instead writing files right away to user this returns an array
 * containing those files. Actually most of code is copied from list_files()
 * function.
 * Differences with list_files:
 *
 *   - file_list("/w"); returns ({ "w" })
 *
 *   - file_list("/w/"); and file_list("/w/."); return contents of directory
 *     "/w"
 *
 *   - file_list("/");, file_list("."); and file_list("/."); return contents
 *     of directory "/"
 *
 * With second argument equal to non-zero, instead of returning an array
 * of strings, the function will return an array of arrays about files.
 * The information in each array is supplied in the order:
 *    name of file,
 *    size of file,
 *    last update of file.
 */
/* WIN32 should be fixed to do this correctly (i.e. no ifdefs for it) */
enum { MAX_FNAME_SIZE = 255, MAX_PATH_LEN = 1024 };
array_t *get_dir(const char *path, int flags) {
  auto max_array_size = CONFIG_INT(__MAX_ARRAY_SIZE__);

  array_t *v;
  DIR *dirp;
  int do_match = 0;

  struct dirent *de;
  struct stat st {};
  std::string temppath;
  std::string regexppath;
  size_t split_pos = 0;

  if (!path) {
    return nullptr;
  }

  path = check_valid_path(path, current_object, "stat", 0);

  if (path == nullptr) {
    return nullptr;
  }

  if (strlen(path) < 2) {
    temppath = path[0] ? std::string(1, path[0]) : ".";
  } else {
    temppath = path;

    /*
     * If path ends with '/' or "/." remove it
     */
    auto last_slash = temppath.find_last_of('/');
    split_pos = (last_slash == std::string::npos) ? 0 : last_slash;
    if (split_pos < temppath.size() && temppath[split_pos] == '/' &&
        ((split_pos + 1 == temppath.size()) ||
         (split_pos + 2 == temppath.size() && temppath[split_pos + 1] == '.'))) {
      temppath.erase(split_pos);
    }
  }

  if (stat(temppath.c_str(), &st) < 0) {
    if (split_pos == temppath.size()) {
      return nullptr;
    }
    if (split_pos != 0) {
      regexppath = temppath.substr(split_pos + 1);
      temppath.erase(split_pos);
    } else {
      regexppath = temppath;
      temppath = ".";
    }
    do_match = 1;
  } else if (split_pos < temppath.size() && temppath != ".") {
    const char *entry_name = temppath.c_str() + split_pos;
    if (*entry_name == '/' && *(entry_name + 1) != '\0') {
      entry_name++;
    }
    v = allocate_empty_array(1);
    encode_stat(&v->item[0], flags, entry_name, &st);
    return v;
  }
  if ((dirp = opendir(temppath.c_str())) == nullptr) {
    return nullptr;
  }
  struct DirEntry {
    std::string name;
    struct stat st {};
  };
  std::vector<DirEntry> entries;
  entries.reserve(std::min(max_array_size, 256));

  for (de = readdir(dirp); de && static_cast<int>(entries.size()) < max_array_size; de = readdir(dirp)) {
    if (!do_match && (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)) {
      continue;
    }
    if (do_match && !match_string(regexppath.c_str(), de->d_name)) {
      continue;
    }

    DirEntry entry;
    entry.name = de->d_name;
    if (flags == -1) {
      auto full_path = append_path_component(temppath.c_str(), de->d_name);
      if (stat(full_path.c_str(), &entry.st) != 0) {
        memset(&entry.st, 0, sizeof(entry.st));
      }
    }
    entries.emplace_back(std::move(entry));
  }
  closedir(dirp);

  std::sort(entries.begin(), entries.end(),
            [](const DirEntry &lhs, const DirEntry &rhs) { return lhs.name < rhs.name; });

  v = allocate_empty_array(entries.size());
  for (size_t i = 0; i < entries.size(); i++) {
    encode_stat(&v->item[i], flags, entries[i].name.c_str(), &entries[i].st);
  }
  return v;
}

int remove_file(const char *path) {
  path = check_valid_path(path, current_object, "remove_file", 1);

  if (path == nullptr) {
    return 0;
  }
  if (unlink(path) == -1) {
    return 0;
  }
  return 1;
}

/*
 * Append string to file. Return 0 for failure, otherwise 1.
 */
int write_file(const char *file, const char *str, int flags) {
  FILE *f;
  gzFile gf;

  file = check_valid_path(file, current_object, "write_file", 1);
  if (!file) {
    return 0;
  }
  if (flags & 2) {
    gf = gzopen(file, (flags & 1) ? "wb" : "ab");
    if (!gf) {
      error("Wrong permissions for opening file /%s for %s.\n\"%s\"\n", file,
            (flags & 1) ? "overwrite" : "append", strerror(errno));
    }
  } else {
    f = fopen(file, (flags & 1) ? "wb" : "ab");
    if (f == nullptr) {
      error("Wrong permissions for opening file /%s for %s.\n\"%s\"\n", file,
            (flags & 1) ? "overwrite" : "append", strerror(errno));
    }
  }
  if (flags & 2) {
    gzwrite(gf, str, strlen(str));
  } else {
    fwrite(str, strlen(str), 1, f);
  }

  if (flags & 2) {
    gzclose(gf);
  } else {
    fclose(f);
  }
  return 1;
}

static bool read_file_is_gzip(const char *file) {
  unsigned char magic[2] = {0, 0};
  FILE *fp = fopen(file, "rb");

  if (!fp) {
    return false;
  }

  size_t read = fread(magic, 1, sizeof(magic), fp);
  fclose(fp);
  return read == sizeof(magic) && magic[0] == 0x1f && magic[1] == 0x8b;
}

static char *copy_read_file_result(const char *buffer, size_t len) {
  bool has_cr = false;
  for (size_t i = 0; i < len; i++) {
    if (buffer[i] == '\r') {
      has_cr = true;
      break;
    }
  }

  if (!has_cr) {
    char *result = new_string(len, "read_file: result");
    memcpy(result, buffer, len);
    result[len] = '\0';
    return result;
  }

  std::string content;
  content.reserve(len);
  bool pending_cr = false;
  for (size_t i = 0; i < len; i++) {
    const char c = buffer[i];
    if (pending_cr) {
      if (c == '\n') {
        content.push_back('\n');
        pending_cr = false;
        continue;
      }
      content.push_back('\r');
      pending_cr = false;
    }

    if (c == '\r') {
      pending_cr = true;
    } else {
      content.push_back(c);
    }
  }

  if (pending_cr) {
    content.push_back('\r');
  }

  return string_copy(content.c_str(), "read file: CRLF result");
}

static char *extract_read_file_slice(const char *file, char *buffer, int total_bytes_read, int start,
                                     int lines, int read_file_max_size) {
  const char *ptr_start = buffer;

  if (start > 1) {
    while (start > 1 && ptr_start < buffer + total_bytes_read) {
      if (*ptr_start == '\0') {
        debug(file, "read_file: file contains '\\0': %s.\n", file);
        return nullptr;
      }
      if (*ptr_start == '\n') {
        start--;
      }
      ptr_start++;
    }

    if (start > 1) {
      debug(file, "read_file: reached EOF searching for start: %s.\n", file);
      return nullptr;
    }
  } else if (start < 0) {
    ptr_start += total_bytes_read - 1;

    if (*ptr_start != '\n') {
      ptr_start++;
    }

    while (start < 0 && ptr_start > buffer) {
      ptr_start--;
      if (*ptr_start == '\0') {
        debug(file, "read_file: file contains '\\0': %s.\n", file);
        return nullptr;
      }
      if (*ptr_start == '\n') {
        start++;
      }
      if (!start) {
        ptr_start++;
      }
    }

    if (start < 0) {
      ptr_start = buffer;
    }
  }

  char *ptr_end = buffer + total_bytes_read;

  if (lines > 0) {
    ptr_end = const_cast<char *>(ptr_start);
    while (lines > 0 && ptr_end <= buffer + total_bytes_read) {
      if (*ptr_end++ == '\n') {
        lines--;
      }
    }
  }

  if (ptr_end > ptr_start + read_file_max_size) {
    ptr_end = const_cast<char *>(ptr_start) + read_file_max_size;
  }

  return copy_read_file_result(ptr_start, ptr_end - ptr_start);
}

static char *read_file_plain_streaming(const char *file, int start, int lines, int read_file_max_size) {
  FILE *fp = fopen(file, "rb");
  if (!fp) {
    debug(file, "read_file: fail to open plain file: %s.\n", file);
    return nullptr;
  }

  std::string content;
  content.reserve(std::min(read_file_max_size, 4096));

  int current_line = 1;
  bool pending_cr = false;
  bool reached_start = (start <= 1);
  char chunk[8192];
  size_t bytes_read = 0;

  while ((bytes_read = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
    for (size_t i = 0; i < bytes_read; i++) {
      const char c = chunk[i];
      if (c == '\0') {
        fclose(fp);
        debug(file, "read_file: file contains '\\0': %s.\n", file);
        return nullptr;
      }

      if (pending_cr) {
        if (c == '\n') {
          if (reached_start && static_cast<int>(content.size()) < read_file_max_size) {
            content.push_back('\n');
          }
          if (reached_start && lines > 0 && --lines == 0) {
            fclose(fp);
            return string_copy(content.c_str(), "read_file: streamed result");
          }
          current_line++;
          reached_start = (current_line >= start);
          pending_cr = false;
          continue;
        }

        if (reached_start && static_cast<int>(content.size()) < read_file_max_size) {
          content.push_back('\r');
        }
        pending_cr = false;
      }

      if (c == '\r') {
        pending_cr = true;
        continue;
      }

      if (reached_start && static_cast<int>(content.size()) < read_file_max_size) {
        content.push_back(c);
      }

      if (c == '\n') {
        if (reached_start && lines > 0 && --lines == 0) {
          fclose(fp);
          return string_copy(content.c_str(), "read_file: streamed result");
        }
        current_line++;
        reached_start = (current_line >= start);
      }

      if (static_cast<int>(content.size()) >= read_file_max_size) {
        fclose(fp);
        return string_copy(content.c_str(), "read_file: streamed result");
      }
    }
  }

  fclose(fp);

  if (pending_cr && reached_start && static_cast<int>(content.size()) < read_file_max_size) {
    content.push_back('\r');
  }

  if (current_line < start) {
    debug(file, "read_file: reached EOF searching for start: %s.\n", file);
    return nullptr;
  }

  return string_copy(content.c_str(), "read_file: streamed result");
}

/* Reads file, starting from line of "start", with maximum lines of "lines".
 * Returns a malloced_string.
 */
char *read_file(const char *file, int start, int lines) {
  const auto read_file_max_size = CONFIG_INT(__MAX_READ_FILE_SIZE__);

  if (lines < 0) {
    debug(file, "read_file: trying to read negative lines: %d", lines);
    return nullptr;
  }

  const char *real_file;

  real_file = check_valid_path(file, current_object, "read_file", 0);
  if (!real_file) {
    return nullptr;
  }

  try {
    auto fs_real_file = fs::u8path(real_file);

    /*
     * file doesn't exist, or is really a directory
     */
    if (!fs::exists(fs_real_file) || fs::is_directory(fs_real_file)) {
      return nullptr;
    }

    if (fs::is_empty(fs_real_file)) {
      /* zero length file */
      char *result = new_string(0, "read_file: empty");
      result[0] = '\0';
      return result;
    }

  } catch (fs::filesystem_error &err) {
    debug(file, "read_file: filesystem error: %s (%d).\n", err.what(), err.code().value());
    return nullptr;
  }

  if (start > 0 && !read_file_is_gzip(real_file)) {
    return read_file_plain_streaming(real_file, start, lines, read_file_max_size);
  }

  gzFile f = gzopen(real_file, "rb");

  if (f == nullptr) {
    debug(file, "read_file: fail to open: %s.\n", file);
    return nullptr;
  }

  static char *the_buff = nullptr;
  if (!the_buff) {
    the_buff = reinterpret_cast<char *>(
        DMALLOC(2 * read_file_max_size + 1, TAG_PERMANENT, "read_file: theBuff"));
  }

  int const total_bytes_read = gzread(f, (void *)the_buff, 2 * read_file_max_size);
  gzclose(f);

  if (total_bytes_read <= 0) {
    debug(file, "read_file: read error: %s.\n", file);
    return nullptr;
  }
  the_buff[total_bytes_read] = '\0';
  return extract_read_file_slice(file, the_buff, total_bytes_read, start, lines, read_file_max_size);
}

char *read_bytes(const char *file, int start, int len, int *rlen) {
  const auto max_byte_transfer = CONFIG_INT(__MAX_BYTE_TRANSFER__);

  struct stat st;
  FILE *fptr;
  char *str;
  int size;

  if (len < 0) {
    return nullptr;
  }
  file = check_valid_path(file, current_object, "read_bytes", 0);
  if (!file) {
    return nullptr;
  }
  fptr = fopen(file, "rb");
  if (fptr == nullptr) {
    return nullptr;
  }
  if (fstat(fileno(fptr), &st) == -1) {
    fatal("Could not stat an open file.\n");
  }
  size = st.st_size;
  if (start < 0) {
    start = size + start;
  }

  if (len == 0) {
    len = size;
  }
  if (len > max_byte_transfer) {
    fclose(fptr);
    error("Transfer exceeded maximum allowed number of bytes.\n");
    return nullptr;
  }
  if (start >= size) {
    fclose(fptr);
    return nullptr;
  }
  if ((start + len) > size) {
    len = (size - start);
  }

  if ((size = fseek(fptr, start, 0)) < 0) {
    fclose(fptr);
    return nullptr;
  }

  str = new_string(len, "read_bytes: str");

  size = fread(str, 1, len, fptr);

  fclose(fptr);

  if (size <= 0) {
    FREE_MSTR(str);
    return nullptr;
  }
  /*
   * The string has to end to '\0'!!!
   */
  str[size] = '\0';

  *rlen = size;
  return str;
}

int write_bytes(const char *file, int start, const char *str, int theLength) {
  const auto max_byte_transfer = CONFIG_INT(__MAX_BYTE_TRANSFER__);

  struct stat st;
  int size;
  FILE *fptr;

  file = check_valid_path(file, current_object, "write_bytes", 1);

  if (!file) {
    return 0;
  }
  if (theLength > max_byte_transfer) {
    return 0;
  }
  /* Under system V, it isn't possible change existing data in a file
   * opened for append, so it can't be opened for append.
   * opening for r+ won't create the file if it doesn't exist.
   * opening for w or w+ will truncate it if it does exist.  So we
   * have to check if it exists first.
   */
  if (stat(file, &st) == -1) {
    fptr = fopen(file, "wb");
  } else {
    fptr = fopen(file, "r+b");
  }
  if (fptr == nullptr) {
    return 0;
  }
  if (fstat(fileno(fptr), &st) == -1) {
    fatal("Could not stat an open file.\n");
  }
  size = st.st_size;
  if (start < 0) {
    start = size + start;
  }
  if (start < 0 || start > size) {
    fclose(fptr);
    return 0;
  }
  if ((size = fseek(fptr, start, 0)) < 0) {
    fclose(fptr);
    return 0;
  }
  size = fwrite(str, 1, theLength, fptr);

  fclose(fptr);

  if (size <= 0) {
    return 0;
  }
  return 1;
}

int file_size(const char *file) {
  struct stat st;
  long ret;

  file = check_valid_path(file, current_object, "file_size", 0);
  if (!file) {
    return -1;
  }

  if (stat(file, &st) == -1) {
    ret = -1;
  } else if (S_IFDIR & st.st_mode) {
    ret = -2;
  } else {
    ret = st.st_size;
  }

  return ret;
}

/*
 * Check that a path to a file is valid for read or write.
 * This is done by functions in the master object.
 * The path is always treated as an absolute path, and is returned without
 * a leading '/'.
 * If the path was '/', then '.' is returned.
 * Otherwise, the returned path is temporarily allocated by apply(), which
 * means it will be deallocated at next apply().
 */
const char *check_valid_path(const char *path, object_t *call_object, const char *const call_fun,
                             int writeflg) {
  svalue_t *v;

  if (!master_ob && !call_object) {
    // early startup, ignore security
    free_svalue(&apply_ret_value, "check_valid_path");
    apply_ret_value.type = T_STRING;
    apply_ret_value.subtype = STRING_MALLOC;
    path = apply_ret_value.u.string = string_copy(path, "check_valid_path");
    return path;
  }

  if (call_object == nullptr || call_object->flags & O_DESTRUCTED) {
    return nullptr;
  }

  copy_and_push_string(path);
  push_object(call_object);
  push_constant_string(call_fun);
  if (writeflg) {
    v = safe_apply_master_ob(APPLY_VALID_WRITE, 3);
  } else {
    v = safe_apply_master_ob(APPLY_VALID_READ, 3);
  }

  if (v == (svalue_t *)-1) {
    v = nullptr;
  }

  if (v && v->type == T_NUMBER && v->u.number == 0) {
    return nullptr;
  }
  if (v && v->type == T_STRING) {
    path = v->u.string;
  } else {
    extern svalue_t apply_ret_value;

    free_svalue(&apply_ret_value, "check_valid_path");
    apply_ret_value.type = T_STRING;
    apply_ret_value.subtype = STRING_MALLOC;
    path = apply_ret_value.u.string = string_copy(path, "check_valid_path");
  }

  if (path[0] == '/') {
    path++;
  }
  if (path[0] == '\0') {
    path = ".";
  }
  if (legal_path(path)) {
    return path;
  }

  return nullptr;
}

static int match_string(const char *match, const char *str) {
  int i;

again:
  if (*str == '\0' && *match == '\0') {
    return 1;
  }
  switch (*match) {
    case '?':
      if (*str == '\0') {
        return 0;
      }
      str++;
      match++;
      goto again;
    case '*':
      match++;
      if (*match == '\0') {
        return 1;
      }
      for (i = 0; str[i] != '\0'; i++) {
        if (match_string(match, str + i)) {
          return 1;
        }
      }
      return 0;
    case '\0':
      return 0;
    case '\\':
      match++;
      if (*match == '\0') {
        return 0;
      }
    /* Fall through ! */
    default:
      if (*match == *str) {
        match++;
        str++;
        goto again;
      }
      return 0;
  }
}

static struct stat to_stats, from_stats;

/* Move FROM onto TO.  Handles cross-filesystem moves.
   If TO is a directory, FROM must be also.
   Return 0 if successful, 1 if an error occurred.  */

#ifdef F_RENAME
static int do_move(const char *from, const char *to, int flag) {
  if (lstat(from, &from_stats) != 0) {
    error("/%s: lstat failed\n", from);
    return 1;
  }
  if (lstat(to, &to_stats) == 0) {
#ifdef __WIN32
    if (strcmp(from, to) == 0) {
#else
    if (from_stats.st_dev == to_stats.st_dev && from_stats.st_ino == to_stats.st_ino) {
#endif
      error("`/%s' and `/%s' are the same file", from, to);
      return 1;
    }
    if (S_ISDIR(to_stats.st_mode)) {
      error("/%s: cannot overwrite directory", to);
      return 1;
    }
  } else if (errno != ENOENT) {
    error("/%s: unknown error\n", to);
    return 1;
  }
  if (flag == F_RENAME) {
    std::error_code error_code;
    fs::rename(from, to, error_code);
    if (!error_code) {
      return 0;
    }
  }
#ifdef F_LINK
  else if (flag == F_LINK) {
    if (link(from, to) == 0) {
      return 0;
    }
  }
#endif

  if (errno != EXDEV) {
    if (flag == F_RENAME) {
      error("cannot move `/%s' to `/%s'\n", from, to);
    } else {
      error("cannot link `/%s' to `/%s'\n", from, to);
    }
    return 1;
  }
  /* rename failed on cross-filesystem link.  Copy the file instead. */
  if (flag == F_RENAME) {
    if (copy_file(from, to)) {
      return 1;
    }
    if (unlink(from)) {
      error("cannot remove `/%s'", from);
      return 1;
    }
  }
#ifdef F_LINK
  else if (flag == F_LINK) {
    if (symlink(from, to) == 0) { /* symbolic link */
      return 0;
    }
  }
#endif
  return 0;
}
#endif

void debug_perror(const char *what, const char *file) {
  if (file) {
    debug_message("System Error: %s:%s:%s\n", what, file, strerror(errno));
  } else {
    debug_message("System Error: %s:%s\n", what, strerror(errno));
  }
}

/*
 * do_rename is used by the efun rename. It is basically a combination
 * of the unix system call rename and the unix command mv.
 */

static svalue_t from_sv = {T_NUMBER};
static svalue_t to_sv = {T_NUMBER};

#ifdef DEBUGMALLOC_EXTENSIONS
void mark_file_sv() {
  mark_svalue(&from_sv);
  mark_svalue(&to_sv);
}
#endif

#ifdef F_RENAME
int do_rename(const char *fr, const char *t, int flag) {
  const char *from;
  const char *to;
  std::string newfrom;
  int flen;
  extern svalue_t apply_ret_value;

  /*
   * important that the same write access checks are done for link() as are
   * done for rename().  Otherwise all kinds of security problems would
   * arise (e.g. creating links to files in protected directories and then
   * modifying the protected file by modifying the linked file). The idea
   * is prevent linking to a file unless the person doing the linking has
   * permission to move the file.
   */
  from = check_valid_path(fr, current_object, "rename", 1);
  if (!from) {
    return 1;
  }

  assign_svalue(&from_sv, &apply_ret_value);

  to = check_valid_path(t, current_object, "rename", 1);
  if (!to) {
    return 1;
  }

  assign_svalue(&to_sv, &apply_ret_value);
  if (!strlen(to) && !strcmp(t, "/")) {
    to = "./";
  }

  /* Strip trailing slashes */
  flen = strlen(from);
  if (flen > 1 && from[flen - 1] == '/') {
    const char *p = from + flen - 2;
    int n;

    while (*p == '/' && (p > from)) {
      p--;
    }
    n = p - from + 1;
    newfrom.assign(from, n);
    from = newfrom.c_str();
  }

  if (file_size(to) == -2) {
    /* Target is a directory; build full target filename. */
    const char *cp;

    cp = strrchr(from, '/');
    if (cp) {
      cp++;
    } else {
      cp = from;
    }

    auto newto = append_path_component(to, cp);
    return do_move(from, newto.c_str(), flag);
  }
  return do_move(from, to, flag);
}
#endif /* F_RENAME */

int copy_file(const char *from, const char *to) {
  extern svalue_t apply_ret_value;

  from = check_valid_path(from, current_object, "move_file", 0);
  assign_svalue(&from_sv, &apply_ret_value);

  to = check_valid_path(to, current_object, "move_file", 1);
  assign_svalue(&to_sv, &apply_ret_value);

  if (from == nullptr) {
    return -1;
  }
  if (to == nullptr) {
    return -2;
  }

  if (lstat(from, &from_stats) != 0) {
    error("/%s: lstat failed\n", from);
    return 1;
  }
  if (lstat(to, &to_stats) == 0) {
#ifdef __WIN32
    if (!strcmp(from, to)) {
#else
    if (from_stats.st_dev == to_stats.st_dev && from_stats.st_ino == to_stats.st_ino) {
#endif
      error("`/%s' and `/%s' are the same file", from, to);
      return 1;
    }
  } else if (errno != ENOENT) {
    error("/%s: unknown error\n", to);
    return 1;
  }

  if (file_size(to) == -2) {
    /* Target is a directory; build full target filename. */
    const char *cp;

    cp = strrchr(from, '/');
    if (cp) {
      cp++;
    } else {
      cp = from;
    }
    auto newto = append_path_component(to, cp);
    return copy_file(from, newto.c_str());
  }

  std::error_code error_code;
  auto base = fs::current_path();
  fs::copy_file(base / from, base / to, fs::copy_options::overwrite_existing, error_code);

  if (error_code) {
    debug_message("Error copying file from /%s to /%s, Error: %s", from, to,
                  error_code.message().c_str());
    return -1;
  }

  return 1;
}

#ifdef F_CP
void f_cp() {
  int i;

  i = copy_file(sp[-1].u.string, sp[0].u.string);
  free_string_svalue(sp--);
  free_string_svalue(sp);
  put_number(i);
}
#endif

#ifdef F_FILE_SIZE
void f_file_size() {
  LPC_INT i = file_size(sp->u.string);

  // cross platform fix
#ifdef _WIN32
  if (i == -1 && sp->u.string[SVALUE_STRLEN(sp) - 1] == '/') {
    auto len = SVALUE_STRLEN(sp);
    auto tmp = string_copy(sp->u.string, "f_file_size");
    tmp[len - 1] = '\0';
    if (file_size(tmp) == -2) {
      i = -2;
    }
    FREE_MSTR(tmp);
  }
#endif

  free_string_svalue(sp);
  put_number(i);
}
#endif

#ifdef F_GET_DIR
void f_get_dir() {
  array_t *vec;

  vec = get_dir((sp - 1)->u.string, sp->u.number);
  free_string_svalue(--sp);
  if (vec) {
    put_array(vec);
  } else {
    *sp = const0;
  }
}
#endif

#ifdef F_LINK
void f_link() {
  svalue_t *ret, *arg;
  int i;

  arg = sp;
  push_svalue(arg - 1);
  push_svalue(arg);
  ret = apply_master_ob(APPLY_VALID_LINK, 2);
  if (MASTER_APPROVED(ret)) {
    i = do_rename((sp - 1)->u.string, sp->u.string, F_LINK);
  } else {
    i = 0;
  }
  (--sp)->type = T_NUMBER;
  sp->u.number = i;
  sp->subtype = 0;
}
#endif /* F_LINK */

#ifdef F_MKDIR
void f_mkdir() {
  const char *path;

  path = check_valid_path(sp->u.string, current_object, "mkdir", 1);
  if (!path || OS_mkdir(path, 0770) == -1) {
    free_string_svalue(sp);
    *sp = const0;
  } else {
    free_string_svalue(sp);
    *sp = const1;
  }
}
#endif
