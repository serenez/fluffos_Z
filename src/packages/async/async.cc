#include "base/package_api.h"

#include "packages/async/async.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if HAVE_DIRENT_H
#include <dirent.h>
#else
#define dirent direct
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

#include <sys/param.h>  // for MAXPATHLEN
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>
#include <vm/internal/base/interpret.h>

#ifdef F_ASYNC_DB_EXEC
#include "packages/db/db.h"
#endif

#include "packages/core/file.h"  // check_valid_path, FIXME

namespace {

enum atypes { AREAD, AWRITE, AGETDIR, ADBEXEC, ADONE };

enum astates { BUSY, DONE };

struct Request {
  std::string path;
  int flags = 0;
  int ret = 0;
  int handle = 0;
  std::string data;
  std::vector<std::string> entries;
  size_t limit = 0;
  function_to_call_t *fun = nullptr;
  struct Request *next = nullptr;
  enum atypes type = AREAD;
  int status = DONE;
};

struct Work {
  struct Request *data;
  void *(*func)(struct Request *);
};

std::deque<struct Work *> reqs;
std::mutex reqs_lock;
std::condition_variable reqs_cv;
std::condition_variable reqs_idle_cv;
std::thread worker_thread;
bool worker_thread_started = false;
bool worker_thread_stopping = false;
bool worker_thread_busy = false;

std::deque<struct Request *> finished_reqs;
std::mutex finished_reqs_lock;
bool callback_event_pending = false;

void thread_func() {
  Tracer::setThreadName("Package Async thread");

  ScopedTracer const tracer("Async thread loop");

  std::unique_lock<std::mutex> lock(reqs_lock);
  while (true) {
    reqs_cv.wait(lock, [] { return worker_thread_stopping || !reqs.empty(); });
    if (worker_thread_stopping && reqs.empty()) {
      worker_thread_started = false;
      worker_thread_busy = false;
      reqs_idle_cv.notify_all();
      return;
    }

    auto *w = reqs.front();
    reqs.pop_front();
    worker_thread_busy = true;
    lock.unlock();

    if (w) {
      {
        ScopedTracer const work_tracer("Async thread work", EventCategory::DEFAULT, [=] {
          return json{{"type", w->data->type}};
        });

        w->func(w->data);
      }
      if (w->data->status == DONE) {
        bool should_schedule = false;
        {
          std::lock_guard<std::mutex> const finished_lock(finished_reqs_lock);
          finished_reqs.push_back(w->data);
          if (!callback_event_pending) {
            callback_event_pending = true;
            should_schedule = true;
          }
        }
        delete w;
        if (should_schedule) {
          add_walltime_event(std::chrono::milliseconds(0), TickEvent::callback_type([] { check_reqs(); }));
        }
      } else {
        std::lock_guard<std::mutex> const req_lock(reqs_lock);
        reqs.push_back(w);
        reqs_cv.notify_one();
      }
    }

    lock.lock();
    worker_thread_busy = false;
    if (reqs.empty()) {
      reqs_idle_cv.notify_all();
    }
  }
}

void do_stuff(void *(*func)(struct Request *), struct Request *data) {
  std::lock_guard<std::mutex> const lock(reqs_lock);

  if (!worker_thread_started) {
    worker_thread_stopping = false;
    worker_thread_started = true;
    worker_thread = std::thread(thread_func);
  }

  auto *i = new Work;
  i->func = func;
  i->data = data;

  reqs.push_back(i);
  reqs_cv.notify_one();
}

void *gzreadthread(struct Request *req) {
  gzFile file = gzopen(req->path.c_str(), "rb");
  if (file == nullptr) {
    req->ret = -1;
    req->data.clear();
    req->status = DONE;
    return nullptr;
  }

  req->ret = gzread(file, (void *)(req->data.data()), req->data.size());
  req->status = DONE;
  gzclose(file);
  return nullptr;
}

int aio_gzread(struct Request *req) {
  req->status = BUSY;
  do_stuff(gzreadthread, req);
  return 0;
}

void *gzwritethread(struct Request *req) {
  int const fd =
      open(req->path.c_str(), req->flags & 1 ? O_CREAT | O_WRONLY | O_TRUNC : O_CREAT | O_WRONLY | O_APPEND,
           S_IRWXU | S_IRWXG);
  if (fd == -1) {
    req->ret = -1;
    req->status = DONE;
    return nullptr;
  }

  gzFile file = gzdopen(fd, "wb");
  if (file == nullptr) {
    close(fd);
    req->ret = -1;
    req->status = DONE;
    return nullptr;
  }

  req->ret = gzwrite(file, (void *)(req->data.data()), req->data.size());
  if (req->ret == 0 && !req->data.empty()) {
    int errnum = Z_OK;
    (void)gzerror(file, &errnum);
    if (errnum != Z_OK) {
      req->ret = -1;
    }
  }
  req->status = DONE;
  gzclose(file);
  return nullptr;
}

int aio_gzwrite(struct Request *req) {
  req->status = BUSY;
  do_stuff(gzwritethread, req);
  return 0;
}

void *writethread(struct Request *req) {
  int const fd =
      open(req->path.c_str(), req->flags & 1 ? O_CREAT | O_WRONLY | O_TRUNC : O_CREAT | O_WRONLY | O_APPEND,
           S_IRWXU | S_IRWXG);
  if (fd == -1) {
    req->ret = -1;
    req->status = DONE;
    return nullptr;
  }

  req->ret = write(fd, req->data.data(), req->data.size());

  req->status = DONE;
  close(fd);
  return nullptr;
}

int aio_write(struct Request *req) {
  req->status = BUSY;
  do_stuff(writethread, req);
  return 0;
}

void *readthread(struct Request *req) {
  int const fd = open(req->path.c_str(), O_RDONLY);
  if (fd == -1) {
    req->ret = -1;
    req->data.clear();
    req->status = DONE;
    return nullptr;
  }

  auto const size = read(fd, (void *)(req->data.data()), req->data.size());
  close(fd);
  if (size < 0) {
    req->ret = -1;
    req->data.clear();
  } else {
    req->data.resize(size);
    req->ret = size;
  }
  req->status = DONE;
  return nullptr;
}

int aio_read(struct Request *req) {
  req->status = BUSY;
  do_stuff(readthread, req);
  return 0;
}

} // namespace

#ifdef F_ASYNC_DB_EXEC
void *dbexecthread(struct Request *req) {
  ScopedTracer const work_tracer("db_exec", EventCategory::DEFAULT, [=] { return json{req->data}; });

  db_t *db = lock_db_conn(req->handle);
  int ret = -1;
  if (db && db->type->execute) {
    if (db->type->cleanup) {
      db->type->cleanup(&(db->c));
    }

    ret = db->type->execute(&(db->c), req->data.c_str());
    if (ret == -1) {
      if (db->type->error) {
        char *tmp = db->type->error(&(db->c));
        req->path = std::string(tmp);
        FREE_MSTR(tmp);
      } else {
        req->path = "Unknown error";
      }
    }
  } else {
    req->path = std::string("No database exec function!");
  }
  unlock_db_conn(db);

  req->ret = ret;
  req->status = DONE;
  return nullptr;
}

int aio_db_exec(struct Request *req) {
  req->status = BUSY;
  do_stuff(dbexecthread, req);
  return 0;
}
#endif

#ifdef F_ASYNC_GETDIR
void *getdirthread(struct Request *req) {
  ScopedTracer const work_tracer("getdir", EventCategory::DEFAULT, [=] { return json{req->path}; });

  DIR *dirp = nullptr;
  if ((dirp = opendir(req->path.c_str())) == nullptr) {
    req->ret = 0;
    req->status = DONE;
    return nullptr;
  }
  req->entries.clear();
  for (auto *de = readdir(dirp); de; de = readdir(dirp)) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
    if (req->entries.size() >= req->limit) {
      break;
    }
    req->entries.emplace_back(de->d_name);
  }

  closedir(dirp);

  std::sort(req->entries.begin(), req->entries.end());
  req->ret = req->entries.size();
  req->status = DONE;
  return nullptr;
}

int aio_getdir(struct Request *req) {
  req->status = BUSY;
  do_stuff(getdirthread, req);
  return 0;
}

#endif

int add_read(const char *fname, function_to_call_t *fun) {
  const auto read_file_max_size = CONFIG_INT(__MAX_READ_FILE_SIZE__);

  if (fname) {
    auto *req = new Request();
    // printf("fname: %s\n", fname);
    req->data.resize(read_file_max_size);
    req->fun = fun;
    req->type = AREAD;
    req->path = std::string(fname);
    return aio_gzread(req);
  }
  error("permission denied\n");

  return 1;
}

#ifdef F_ASYNC_GETDIR
int add_getdir(const char *fname, function_to_call_t *fun) {
  auto max_array_size = CONFIG_INT(__MAX_ARRAY_SIZE__);

  if (fname) {
    // printf("fname: %s\n", fname);
    auto *req = new Request();
    req->limit = max_array_size;
    req->fun = fun;
    req->type = AGETDIR;
    req->path = fname;
    return aio_getdir(req);
  }
  error("permission denied\n");

  return 1;
}
#endif

int add_write(const char *fname, const char *buf, int size, char flags, function_to_call_t *fun) {
  if (!fname) {
    error("permission denied\n");
  }

  auto *req = new Request();
  req->data = std::string(buf, size);
  req->fun = fun;
  req->type = AWRITE;
  req->flags = flags;
  req->path = std::string(fname);
  if (flags & 2) {
    return aio_gzwrite(req);
  }
  return aio_write(req);
}

#ifdef F_ASYNC_DB_EXEC
int add_db_exec(int handle, const char *sql, function_to_call_t *fun) {
  auto *req = new Request();
  req->fun = fun;
  req->type = ADBEXEC;
  req->handle = handle;
  req->data = sql;
  return aio_db_exec(req);
}
#endif

void handle_read(struct Request *req) {
  int const val = req->ret;
  if (val < 0) {
    push_number(val);
    set_eval(max_eval_cost);
    safe_call_efun_callback(req->fun, 1);
    return;
  }
  char *file = new_string(val, "read_file_async: str");
  memcpy(file, (char *)(req->data.data()), val);
  file[val] = 0;
  push_malloced_string(file);
  set_eval(max_eval_cost);
  safe_call_efun_callback(req->fun, 1);
}

#ifdef F_ASYNC_GETDIR
void handle_getdir(struct Request *req) {
  int ret_size = req->entries.size();
  array_t *ret = allocate_empty_array(ret_size);
  if (ret_size > 0) {
    for (int i = 0; i < ret_size; i++) {
      svalue_t *vp = &(ret->item[i]);
      vp->type = T_STRING;
      vp->subtype = STRING_MALLOC;
      vp->u.string = string_copy(req->entries[i].c_str(), "encode_stat");
    }
  }

  push_refed_array(ret);
  set_eval(max_eval_cost);
  safe_call_efun_callback(req->fun, 1);
}
#endif

void handle_write(struct Request *req) {
  int const val = req->ret;
  if (val < 0) {
    push_number(val);
    set_eval(max_eval_cost);
    safe_call_efun_callback(req->fun, 1);
    return;
  }
  push_undefined();
  set_eval(max_eval_cost);
  safe_call_efun_callback(req->fun, 1);
}

void handle_db_exec(struct Request *req) {
  int const val = req->ret;
  if (val == -1) {
    copy_and_push_string(req->path.c_str());
  } else {
    push_number(val);
  }
  set_eval(max_eval_cost);
  safe_call_efun_callback(req->fun, 1);
}

void check_reqs() {
  ScopedTracer const tracer("Async callback");

  std::deque<struct Request *> ready;
  {
    std::lock_guard<std::mutex> const lock(finished_reqs_lock);
    callback_event_pending = false;
    ready.swap(finished_reqs);
  }

  while (!ready.empty()) {
    auto *req = ready.front();
    ready.pop_front();

    enum atypes const type = (req->type);
    req->type = ADONE;
    switch (type) {
      case AREAD:
        handle_read(req);
        break;
      case AWRITE:
        handle_write(req);
        break;
#ifdef F_ASYNC_GETDIR
      case AGETDIR:
        handle_getdir(req);
        break;
#endif
#ifdef F_ASYNC_DB_EXEC
      case ADBEXEC:
        handle_db_exec(req);
        break;
#endif
      case ADONE:
        // must have had an error while handling it before.
        break;
      default:
        fatal("unknown async type\n");
    }
    free_funp(req->fun->f.fp);
    delete req->fun;
    delete req;
  }
}

void complete_all_asyncio() {
  {
    std::unique_lock<std::mutex> lock(reqs_lock);
    reqs_idle_cv.wait(lock, [] { return reqs.empty() && !worker_thread_busy; });
    worker_thread_stopping = true;
    reqs_cv.notify_one();
  }

  if (worker_thread.joinable()) {
    worker_thread.join();
  }
  check_reqs();
}

#ifdef F_ASYNC_READ

void f_async_read() {
  std::unique_ptr<function_to_call_t> cb(new function_to_call_t);
  process_efun_callback(1, cb.get(), F_ASYNC_READ);
  cb->f.fp->hdr.ref++;
  pop_stack();

  add_read(check_valid_path(sp->u.string, current_object, "read_file", 0), cb.release());
  pop_stack();
}
#endif

#ifdef F_ASYNC_WRITE
void f_async_write() {
  std::unique_ptr<function_to_call_t> cb(new function_to_call_t);
  process_efun_callback(3, cb.get(), F_ASYNC_WRITE);
  cb->f.fp->hdr.ref++;
  pop_stack();

  add_write(check_valid_path((sp - 2)->u.string, current_object, "write_file", 1),
            (sp - 1)->u.string, SVALUE_STRLEN((sp - 1)), sp->u.number, cb.release());
  pop_3_elems();
}
#endif

#ifdef F_ASYNC_GETDIR
void f_async_getdir() {
  std::unique_ptr<function_to_call_t> cb(new function_to_call_t);
  process_efun_callback(1, cb.get(), F_ASYNC_GETDIR);
  cb->f.fp->hdr.ref++;
  pop_stack();

  add_getdir(check_valid_path(sp->u.string, current_object, "get_dir", 0), cb.release());
  pop_stack();
}
#endif
#ifdef F_ASYNC_DB_EXEC
void f_async_db_exec() {
  std::unique_ptr<function_to_call_t> cb(new function_to_call_t);
  process_efun_callback(2, cb.get(), F_ASYNC_DB_EXEC);
  cb->f.fp->hdr.ref++;
  pop_stack();

  array_t *info;
  info = allocate_empty_array(1);
  info->item[0].type = T_STRING;
  info->item[0].subtype = STRING_MALLOC;
  info->item[0].u.string = string_copy(sp->u.string, "f_db_exec");
  valid_database("exec", info);

  db_t *db = lock_db_conn((sp - 1)->u.number);
  if (!db) {
    error("Attempt to exec on an invalid database handle\n");
  }
  unlock_db_conn(db);
  add_db_exec((sp - 1)->u.number, sp->u.string, cb.release());
  pop_2_elems();
}
#endif

void async_mark_request() {
#ifdef DEBUGMALLOC_EXTENSIONS
  std::lock_guard<std::mutex> const lock(reqs_lock);
  std::lock_guard<std::mutex> const flock(finished_reqs_lock);

  for (auto &work : reqs) {
    auto *req = work->data;
    if (req->fun != nullptr) {
      req->fun->f.fp->hdr.extra_ref++;
    }
  }

  for (auto &req : finished_reqs) {
    if (req->fun != nullptr) {
      req->fun->f.fp->hdr.extra_ref++;
    }
  }
#endif
}
