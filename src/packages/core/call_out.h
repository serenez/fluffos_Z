#ifndef CALL_OUT_H
#define CALL_OUT_H

/*
 * call_out.c
 */

#include <chrono>
#include <cstddef>
#ifdef TIME_WITH_SYS_TIME
#include <sys/time.h>
#include <time.h>
#else
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else
#include <time.h>
#endif
#endif

typedef struct pending_call_s {
  uint64_t target_time;
  union string_or_func function;
  object_t *ob;
  object_t *owner_bucket_key;
  array_t *vs;
  object_t *command_giver;
  LPC_INT handle;
  struct TickEvent *tick_event;
  struct pending_call_s *owner_prev;
  struct pending_call_s *owner_next;
  struct pending_call_s *pool_next;
  bool is_walltime;
} pending_call_t;

void call_out(pending_call_t *cop);

/* only at exit */
void clear_call_outs(void);

void reclaim_call_outs(void);
int find_call_out_by_handle(object_t *, LPC_INT);
int remove_call_out_by_handle(object_t *, LPC_INT);
LPC_INT new_call_out(object_t *, svalue_t *, std::chrono::milliseconds delay_msec, int, svalue_t *,
                     bool walltime = false);
int remove_call_out(object_t *, const char *);
void remove_all_call_out(object_t *);
int find_call_out(object_t *, const char *);
array_t *get_all_call_outs(void);
int print_call_out_usage(outbuffer_t *, int);
int total_callout_size();
void mark_call_outs(void);
void reclaim_call_outs(void);
size_t call_out_owner_index_entry_count_for_test();
size_t call_out_owner_index_bucket_count_for_test();
size_t call_out_pool_entry_count_for_test();
void clear_call_out_pool_for_test();
void set_call_out_pool_enabled_for_test(bool enabled);

#endif
