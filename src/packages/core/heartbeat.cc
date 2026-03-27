/*
 * heartbeat.cc
 */

#include "base/package_api.h"

#include "packages/core/heartbeat.h"

#include <algorithm>
#include <limits>
#include <set>
#include <unordered_map>
#include <vector>

struct heart_beat_t;

struct heartbeat_queue_t {
  heart_beat_t *head = nullptr;
  heart_beat_t *tail = nullptr;
  size_t size = 0;
};

enum class heart_beat_state_t { kPending, kEveryRound, kScheduled, kCurrentRound, kRunning };

struct heart_beat_t {
  object_t *ob = nullptr;
  short time_to_heart_beat = 0;
  uint64_t due_tick = 0;
  uint64_t seq = 0;
  uint64_t last_processed_tick = std::numeric_limits<uint64_t>::max();
  bool pending_remove = false;
  heart_beat_state_t state = heart_beat_state_t::kPending;
  heart_beat_t *prev = nullptr;
  heart_beat_t *next = nullptr;
};

using heartbeat_schedule_t = std::unordered_map<uint64_t, heartbeat_queue_t>;

// Global pointer to current object executing heartbeat.
object_t *g_current_heartbeat_obj;
static heart_beat_t *g_current_heartbeat_entry;

static heartbeat_queue_t heartbeats_every_round;
static heartbeat_queue_t heartbeats_current_round;
static heartbeat_queue_t heartbeats_pending;
static heartbeat_schedule_t heartbeats_sparse;
static std::unordered_map<object_t *, heart_beat_t> heartbeat_entries;
static uint64_t next_heartbeat_seq = 1;
static uint64_t current_heartbeat_round = 0;

namespace {
inline bool heartbeat_queue_empty(const heartbeat_queue_t &queue) { return queue.head == nullptr; }

void heartbeat_queue_reset_links(heart_beat_t *hb) {
  hb->prev = nullptr;
  hb->next = nullptr;
}

void heartbeat_queue_append_detached(heartbeat_queue_t &queue, heart_beat_t *hb) {
  hb->prev = queue.tail;
  hb->next = nullptr;
  if (queue.tail != nullptr) {
    queue.tail->next = hb;
  } else {
    queue.head = hb;
  }
  queue.tail = hb;
  queue.size++;
}

void heartbeat_queue_insert_before(heartbeat_queue_t &queue, heart_beat_t *pos, heart_beat_t *hb) {
  hb->next = pos;
  hb->prev = pos->prev;
  if (pos->prev != nullptr) {
    pos->prev->next = hb;
  } else {
    queue.head = hb;
  }
  pos->prev = hb;
  queue.size++;
}

void heartbeat_queue_push_back(heartbeat_queue_t &queue, heart_beat_t *hb) {
  heartbeat_queue_reset_links(hb);
  heartbeat_queue_append_detached(queue, hb);
}

void heartbeat_queue_erase(heartbeat_queue_t &queue, heart_beat_t *hb) {
  if (hb->prev != nullptr) {
    hb->prev->next = hb->next;
  } else {
    queue.head = hb->next;
  }
  if (hb->next != nullptr) {
    hb->next->prev = hb->prev;
  } else {
    queue.tail = hb->prev;
  }
  heartbeat_queue_reset_links(hb);
  queue.size--;
}

heart_beat_t *heartbeat_queue_pop_front(heartbeat_queue_t &queue) {
  auto *hb = queue.head;
  heartbeat_queue_erase(queue, hb);
  return hb;
}

void heartbeat_queue_insert_by_seq(heartbeat_queue_t &queue, heart_beat_t *hb) {
  heartbeat_queue_reset_links(hb);
  auto *it = queue.tail;
  while (it != nullptr && it->seq > hb->seq) {
    it = it->prev;
  }
  if (it == nullptr) {
    if (queue.head != nullptr) {
      heartbeat_queue_insert_before(queue, queue.head, hb);
      return;
    }
    heartbeat_queue_append_detached(queue, hb);
    return;
  }
  if (it == queue.tail) {
    heartbeat_queue_append_detached(queue, hb);
    return;
  }
  heartbeat_queue_insert_before(queue, it->next, hb);
}

void heartbeat_queue_merge_by_seq(heartbeat_queue_t &dst, heartbeat_queue_t &src) {
  if (heartbeat_queue_empty(src)) {
    return;
  }
  if (heartbeat_queue_empty(dst)) {
    dst = src;
    src = {};
    return;
  }

  heartbeat_queue_t merged{};
  auto *left = dst.head;
  auto *right = src.head;

  while (left != nullptr || right != nullptr) {
    heart_beat_t *next = nullptr;
    if (right == nullptr || (left != nullptr && left->seq <= right->seq)) {
      next = left;
      left = left->next;
    } else {
      next = right;
      right = right->next;
    }
    heartbeat_queue_reset_links(next);
    heartbeat_queue_append_detached(merged, next);
  }

  dst = merged;
  src = {};
}

template <typename Fn>
void heartbeat_queue_for_each(const heartbeat_queue_t &queue, Fn &&fn) {
  for (auto *hb = queue.head; hb != nullptr; hb = hb->next) {
    fn(hb);
  }
}

void heartbeat_queue_mark_state(heartbeat_queue_t &queue, heart_beat_state_t state) {
  heartbeat_queue_for_each(queue, [&](heart_beat_t *hb) { hb->state = state; });
}

inline bool is_current_heartbeat_target(object_t *ob) {
  return g_current_heartbeat_entry != nullptr && g_current_heartbeat_obj == ob &&
         g_current_heartbeat_entry->ob == ob && !g_current_heartbeat_entry->pending_remove;
}

heart_beat_t *find_heartbeat_entry(object_t *ob) {
  auto it = heartbeat_entries.find(ob);
  if (it == heartbeat_entries.end()) {
    return nullptr;
  }
  return &it->second;
}

bool heartbeat_visible(const heart_beat_t *hb) {
  return hb != nullptr && hb->ob != nullptr && !hb->pending_remove &&
         (hb->ob->flags & O_HEART_BEAT) && !(hb->ob->flags & O_DESTRUCTED);
}

void erase_pending_entry(heart_beat_t *hb) {
  if (hb->state == heart_beat_state_t::kPending) {
    heartbeat_queue_erase(heartbeats_pending, hb);
  }
}

void erase_current_round_entry(heart_beat_t *hb) {
  if (hb->state == heart_beat_state_t::kCurrentRound) {
    heartbeat_queue_erase(heartbeats_current_round, hb);
  }
}

void erase_every_round_entry(heart_beat_t *hb) {
  if (hb->state == heart_beat_state_t::kEveryRound) {
    heartbeat_queue_erase(heartbeats_every_round, hb);
  }
}

void erase_scheduled_entry(heart_beat_t *hb) {
  if (hb->state != heart_beat_state_t::kScheduled) {
    return;
  }

  auto bucket_it = heartbeats_sparse.find(hb->due_tick);
  if (bucket_it == heartbeats_sparse.end()) {
    return;
  }

  heartbeat_queue_erase(bucket_it->second, hb);
  if (heartbeat_queue_empty(bucket_it->second)) {
    heartbeats_sparse.erase(bucket_it);
  }
}

void erase_heartbeat_entry(object_t *ob) {
  auto it = heartbeat_entries.find(ob);
  if (it == heartbeat_entries.end()) {
    return;
  }

  auto *hb = &it->second;
  erase_pending_entry(hb);
  erase_current_round_entry(hb);
  erase_every_round_entry(hb);
  erase_scheduled_entry(hb);
  heartbeat_entries.erase(it);
}

uint64_t schedule_tick_for_existing_entry(const heart_beat_t *hb, int interval) {
  if (hb->last_processed_tick == current_heartbeat_round) {
    return current_heartbeat_round + interval;
  }

  if (g_current_heartbeat_entry != nullptr && hb->seq < g_current_heartbeat_entry->seq) {
    return current_heartbeat_round + interval;
  }

  return current_heartbeat_round + interval - 1;
}

void schedule_current_round_entry(heart_beat_t *hb) {
  hb->due_tick = current_heartbeat_round;
  hb->state = heart_beat_state_t::kCurrentRound;
  heartbeat_queue_insert_by_seq(heartbeats_current_round, hb);
}

void schedule_every_round_entry(heart_beat_t *hb) {
  hb->due_tick = current_heartbeat_round + 1;
  hb->state = heart_beat_state_t::kEveryRound;
  heartbeat_queue_insert_by_seq(heartbeats_every_round, hb);
}

void schedule_sparse_entry(heart_beat_t *hb, uint64_t due_tick) {
  hb->due_tick = due_tick;
  hb->state = heart_beat_state_t::kScheduled;
  auto &bucket = heartbeats_sparse[due_tick];
  heartbeat_queue_insert_by_seq(bucket, hb);
}

void schedule_updated_entry(heart_beat_t *hb, int interval) {
  auto due_tick = schedule_tick_for_existing_entry(hb, interval);
  if (interval == 1) {
    if (due_tick == current_heartbeat_round) {
      schedule_current_round_entry(hb);
    } else {
      schedule_every_round_entry(hb);
    }
    return;
  }
  schedule_sparse_entry(hb, due_tick);
}

void activate_pending_heartbeats() {
  while (!heartbeat_queue_empty(heartbeats_pending)) {
    auto *hb = heartbeat_queue_pop_front(heartbeats_pending);
    hb->state = heart_beat_state_t::kRunning;
    if (!heartbeat_visible(hb)) {
      if (hb->ob != nullptr) {
        erase_heartbeat_entry(hb->ob);
      }
      continue;
    }
    if (hb->time_to_heart_beat == 1) {
      schedule_every_round_entry(hb);
    } else {
      schedule_sparse_entry(hb, current_heartbeat_round + hb->time_to_heart_beat);
    }
  }
}

size_t heartbeat_entry_count() {
  size_t count = 0;
  for (auto &[ob, hb] : heartbeat_entries) {
    (void)ob;
    if (heartbeat_visible(&hb)) {
      count++;
    }
  }
  return count;
}
}  // namespace

/* Call all heart_beat() functions in all objects.
 *
 * Set command_giver to current_object if it is a living object. If the object
 * is shadowed, check the shadowed object if living. There is no need to save
 * the value of the command_giver, as the caller resets it to 0 anyway.  */

namespace {
void call_heart_beat_impl(bool schedule_next_cycle) {
  if (schedule_next_cycle) {
    add_gametick_event(
        time_to_next_gametick(std::chrono::milliseconds(CONFIG_INT(__RC_HEARTBEAT_INTERVAL_MSEC__))),
        TickEvent::callback_type(call_heart_beat));
  }

  if (!heartbeat_queue_empty(heartbeats_every_round)) {
    heartbeat_queue_mark_state(heartbeats_every_round, heart_beat_state_t::kCurrentRound);
    heartbeat_queue_merge_by_seq(heartbeats_current_round, heartbeats_every_round);
  }

  auto sparse_it = heartbeats_sparse.find(current_heartbeat_round);
  if (sparse_it != heartbeats_sparse.end()) {
    heartbeat_queue_mark_state(sparse_it->second, heart_beat_state_t::kCurrentRound);
    heartbeat_queue_merge_by_seq(heartbeats_current_round, sparse_it->second);
    heartbeats_sparse.erase(sparse_it);
  }

  while (!heartbeat_queue_empty(heartbeats_current_round)) {
    auto *hb = heartbeat_queue_pop_front(heartbeats_current_round);
    hb->state = heart_beat_state_t::kRunning;

    auto *ob = hb->ob;
    if (!heartbeat_visible(hb)) {
      if (ob != nullptr) {
        erase_heartbeat_entry(ob);
      }
      continue;
    }

    hb->last_processed_tick = current_heartbeat_round;
    g_current_heartbeat_obj = ob;
    g_current_heartbeat_entry = hb;

    // No heartbeat function; keep the scheduler entry alive for compat.
    if (ob->prog->heart_beat == 0) {
      g_current_heartbeat_entry = nullptr;
      g_current_heartbeat_obj = nullptr;
      if (heartbeat_visible(hb)) {
        if (hb->time_to_heart_beat == 1) {
          schedule_every_round_entry(hb);
        } else {
          schedule_sparse_entry(hb, current_heartbeat_round + hb->time_to_heart_beat);
        }
      } else {
        erase_heartbeat_entry(ob);
      }
      continue;
    }

    object_t *new_command_giver;

    new_command_giver = ob;
#ifndef NO_SHADOWS
    while (new_command_giver->shadowing) {
      new_command_giver = new_command_giver->shadowing;
    }
#endif
#ifndef NO_ADD_ACTION
    if (!(new_command_giver->flags & O_ENABLE_COMMANDS)) {
      new_command_giver = nullptr;
    }
#endif
#ifdef PACKAGE_MUDLIB_STATS
    add_heart_beats(&ob->stats, 1);
#endif
    save_command_giver(new_command_giver);

    // note, NOT same as new_command_giver
    current_interactive = nullptr;
    if (ob->interactive) {
      current_interactive = ob;
    }

    error_context_t econ;

    save_context(&econ);
    try {
      set_eval(max_eval_cost);
      // TODO: provide a safe_call_direct()
      call_direct(ob, ob->prog->heart_beat - 1, ORIGIN_DRIVER, 0);
      pop_stack(); /* pop the return value */
    } catch (const char *) {
      restore_context(&econ);
    }
    pop_context(&econ);

    restore_command_giver();
    current_interactive = nullptr;
    if (heartbeat_visible(hb)) {
      if (hb->time_to_heart_beat == 1) {
        schedule_every_round_entry(hb);
      } else {
        schedule_sparse_entry(hb, current_heartbeat_round + hb->time_to_heart_beat);
      }
    } else {
      erase_heartbeat_entry(ob);
    }
    g_current_heartbeat_entry = nullptr;
    g_current_heartbeat_obj = nullptr;
  }

  activate_pending_heartbeats();
  current_heartbeat_round++;
}
}  // namespace

void call_heart_beat() { call_heart_beat_impl(true); }

void run_heartbeat_cycle_for_test() { call_heart_beat_impl(false); }

// Query heartbeat interval for a object
int query_heart_beat(object_t *ob) {
  if (!(ob->flags & O_HEART_BEAT)) {
    return 0;
  }

  if (is_current_heartbeat_target(ob)) {
    return g_current_heartbeat_entry->time_to_heart_beat;
  }

  auto *hb = find_heartbeat_entry(ob);
  if (heartbeat_visible(hb)) {
    return hb->time_to_heart_beat;
  }

  return 0;
} /* query_heart_beat() */

// Modifying heartbeat for a object.
//
// NOTE: This may get called during heartbeat. Care must be taken to
// make sure it works.
//
// Removing heartbeat just need to remove the flag from objects.
// New heartbeats are staged in heartbeats_pending until the current cycle ends.
int set_heart_beat(object_t *ob, int to) {
  if (ob->flags & O_DESTRUCTED) {
    return 0;
  }

  // This was done in previous driver code, keep here for compat.
  if (to < 0) {
    // TODO: log a warning
    to = 1;
  }

  // Here are 3 possible cases:
  // 1) Object is modifying itself during its heartbeat execution.
  // 2) Object currently have heartbeat and is in the queue.
  // 3) Object currently doesn't have heartbeat.

  auto *target_hb = find_heartbeat_entry(ob);

  // Removal: keep the current running entry alive until callback exit, but
  // remove all non-running entries immediately.
  if (to == 0) {
    ob->flags &= ~O_HEART_BEAT;

    if (target_hb == nullptr) {
      return 0;
    }
    if (target_hb == g_current_heartbeat_entry) {
      target_hb->pending_remove = true;
      return 1;
    }
    erase_heartbeat_entry(ob);
    return 1;
  }

  ob->flags |= O_HEART_BEAT;

  if (target_hb != nullptr) {
    target_hb->ob = ob;
    target_hb->pending_remove = false;
    target_hb->time_to_heart_beat = to;

    if (target_hb == g_current_heartbeat_entry) {
      return 1;
    }

    if (target_hb->state == heart_beat_state_t::kPending) {
      return 1;
    }

    erase_current_round_entry(target_hb);
    erase_every_round_entry(target_hb);
    erase_scheduled_entry(target_hb);
    schedule_updated_entry(target_hb, to);
    return 1;
  }

  auto [it, inserted] = heartbeat_entries.emplace(ob, heart_beat_t{});
  auto *new_hb = &it->second;
  new_hb->ob = ob;
  new_hb->time_to_heart_beat = to;
  new_hb->due_tick = 0;
  new_hb->seq = next_heartbeat_seq++;
  new_hb->last_processed_tick = std::numeric_limits<uint64_t>::max();
  new_hb->pending_remove = false;
  new_hb->state = heart_beat_state_t::kPending;
  heartbeat_queue_push_back(heartbeats_pending, new_hb);
  return 1;
}

int heart_beat_status(outbuffer_t *buf, int verbose) {
  auto count = heartbeat_entry_count();
  if (verbose == 1) {
    outbuf_add(buf, "Heart beat information:\n");
    outbuf_add(buf, "-----------------------\n");
    outbuf_addv(buf, "Number of objects with heart beat: %" PRIu64 ".\n", count);
  }
  return count * static_cast<int>(sizeof(heart_beat_t));
} /* heart_beat_status() */

#ifdef F_HEART_BEATS
array_t *get_heart_beats() {
  std::vector<object_t *> result;
  result.reserve(heartbeat_entry_count());

  bool display_hidden = true;
#ifdef F_SET_HIDE
  display_hidden = valid_hide(current_object);
#endif

  for (auto &[ob, hb] : heartbeat_entries) {
    (void)ob;
    if (!heartbeat_visible(&hb)) {
      continue;
    }
    if (hb.ob->flags & O_HIDDEN) {
      if (!display_hidden) {
        continue;
      }
    }
    result.push_back(hb.ob);
  }

  array_t *arr = allocate_empty_array(result.size());
  int i = 0;
  for (auto *obj : result) {
    arr->item[i].type = T_OBJECT;
    arr->item[i].u.ob = obj;
    add_ref(arr->item[i].u.ob, "get_heart_beats");
    i++;
  }
  return arr;
}
#endif

void check_heartbeats() {
  std::set<object_t *> objset;
  auto insert_entry = [&](heart_beat_t *hb) {
    if (!heartbeat_visible(hb)) {
      return;
    }
    DEBUG_CHECK(!objset.insert(hb->ob).second, "Driver BUG: Duplicated/Missing heartbeats found");
  };

  if (heartbeat_visible(g_current_heartbeat_entry)) {
    insert_entry(g_current_heartbeat_entry);
  }
  heartbeat_queue_for_each(heartbeats_current_round, insert_entry);
  heartbeat_queue_for_each(heartbeats_every_round, insert_entry);
  heartbeat_queue_for_each(heartbeats_pending, insert_entry);
  for (auto &[due_tick, bucket] : heartbeats_sparse) {
    (void)due_tick;
    heartbeat_queue_for_each(bucket, insert_entry);
  }
}

void clear_heartbeats() {
  // TODO: instead of clearing everything blindly, should go through all objects with heartbeat flag
  // and delete corresponding heartbeats, thus exposing leftovers.
  heartbeats_every_round = {};
  heartbeats_current_round = {};
  heartbeats_pending = {};
  heartbeats_sparse.clear();
  heartbeat_entries.clear();
  next_heartbeat_seq = 1;
  current_heartbeat_round = 0;
  g_current_heartbeat_entry = nullptr;
  g_current_heartbeat_obj = nullptr;
}
