/*
 * heartbeat.cc
 */

#include "base/package_api.h"

#include "packages/core/heartbeat.h"

#include <algorithm>
#include <deque>
#include <set>
#include <unordered_map>
#include <vector>

struct heart_beat_t {
  object_t *ob;              // nullptr also means deleted entries.
  short heart_beat_ticks;    // remaining ticks
  short time_to_heart_beat;  // interval
};

// Global pointer to current object executing heartbeat.
object_t *g_current_heartbeat_obj;
static heart_beat_t *g_current_heartbeat_entry;

static std::deque<heart_beat_t> heartbeats;
static std::deque<heart_beat_t> heartbeats_pending;
static std::unordered_map<object_t *, short> heartbeat_intervals;

namespace {
inline bool is_current_heartbeat_target(object_t *ob) {
  return g_current_heartbeat_entry != nullptr && g_current_heartbeat_obj == ob;
}

heart_beat_t *find_heartbeat_entry(object_t *ob) {
  if (is_current_heartbeat_target(ob)) {
    return g_current_heartbeat_entry;
  }
  for (auto &hb : heartbeats) {
    if (hb.ob == ob) {
      return &hb;
    }
  }
  for (auto &hb : heartbeats_pending) {
    if (hb.ob == ob) {
      return &hb;
    }
  }
  return nullptr;
}
}  // namespace

/* Call all heart_beat() functions in all objects.
 *
 * Set command_giver to current_object if it is a living object. If the object
 * is shadowed, check the shadowed object if living. There is no need to save
 * the value of the command_giver, as the caller resets it to 0 anyway.  */

void call_heart_beat() {
  // Register for next call
  add_gametick_event(
      time_to_next_gametick(std::chrono::milliseconds(CONFIG_INT(__RC_HEARTBEAT_INTERVAL_MSEC__))),
      TickEvent::callback_type(call_heart_beat));

  auto cycle_size = heartbeats.size();
  while (cycle_size-- > 0) {
    auto hb = heartbeats.front();
    heartbeats.pop_front();

    auto *ob = hb.ob;
    if (ob == nullptr) {
      continue;
    }

    if (!(ob->flags & O_HEART_BEAT) || ob->flags & O_DESTRUCTED) {
      heartbeat_intervals.erase(ob);
      continue;
    }

    g_current_heartbeat_obj = ob;
    g_current_heartbeat_entry = &hb;

    if (--hb.heart_beat_ticks > 0) {
      g_current_heartbeat_entry = nullptr;
      g_current_heartbeat_obj = nullptr;
      heartbeats.push_back(hb);
      continue;
    }
    hb.heart_beat_ticks = hb.time_to_heart_beat;

    // No heartbeat function
    if (ob->prog->heart_beat == 0) {
      g_current_heartbeat_entry = nullptr;
      g_current_heartbeat_obj = nullptr;
      heartbeats.push_back(hb);
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
    if (hb.ob && (hb.ob->flags & O_HEART_BEAT) && !(hb.ob->flags & O_DESTRUCTED)) {
      heartbeat_intervals[hb.ob] = hb.time_to_heart_beat;
      heartbeats.push_back(hb);
    } else {
      heartbeat_intervals.erase(ob);
    }
    g_current_heartbeat_entry = nullptr;
    g_current_heartbeat_obj = nullptr;
  }

  if (!heartbeats_pending.empty()) {
    for (auto &hb : heartbeats_pending) {
      if (hb.ob && (hb.ob->flags & O_HEART_BEAT) && !(hb.ob->flags & O_DESTRUCTED)) {
        heartbeats.push_back(hb);
      }
    }
    heartbeats_pending.clear();
  }
} /* call_heart_beat() */

// Query heartbeat interval for a object
// NOTE: Not a very efficient function.
int query_heart_beat(object_t *ob) {
  if (!(ob->flags & O_HEART_BEAT)) {
    return 0;
  }

  if (is_current_heartbeat_target(ob) && g_current_heartbeat_entry->ob == ob) {
    return g_current_heartbeat_entry->time_to_heart_beat;
  }

  auto it = heartbeat_intervals.find(ob);
  if (it != heartbeat_intervals.end()) {
    return it->second;
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

  // Removal: set the flag and hb will be deleted in next round.
  if (to == 0) {
    ob->flags &= ~O_HEART_BEAT;
    heartbeat_intervals.erase(ob);

    bool found = false;
    if (is_current_heartbeat_target(ob)) {
      g_current_heartbeat_entry->ob = nullptr;
      found = true;
    }
    for (auto &hb : heartbeats) {
      if (hb.ob == ob) {
        hb.ob = nullptr;
        found = true;
      }
    }
    for (auto &hb : heartbeats_pending) {
      if (hb.ob == ob) {
        hb.ob = nullptr;
        found = true;
      }
    }
    return found ? 1 : 0;
  }
  ob->flags |= O_HEART_BEAT;
  heartbeat_intervals[ob] = to;

  heart_beat_t *target_hb = find_heartbeat_entry(ob);
  if (target_hb != nullptr) {
    target_hb->ob = ob;
    target_hb->time_to_heart_beat = to;
    target_hb->heart_beat_ticks = to;
    return 1;
  }

  auto &new_hb = heartbeats_pending.emplace_back();
  new_hb.ob = ob;
  new_hb.time_to_heart_beat = to;
  new_hb.heart_beat_ticks = to;
  return 1;
}

int heart_beat_status(outbuffer_t *buf, int verbose) {
  if (verbose == 1) {
    outbuf_add(buf, "Heart beat information:\n");
    outbuf_add(buf, "-----------------------\n");
    outbuf_addv(buf, "Number of objects with heart beat: %" PRIu64 ".\n",
                heartbeats.size() + heartbeats_pending.size() +
                    (g_current_heartbeat_entry && g_current_heartbeat_entry->ob ? 1 : 0));
  }
  // may overcount, but this usually not called during heartbeat.
  return (heartbeats.size() + heartbeats_pending.size() +
          (g_current_heartbeat_entry && g_current_heartbeat_entry->ob ? 1 : 0)) *
         (sizeof(heart_beat_t *) + sizeof(heart_beat_t));
} /* heart_beat_status() */

#ifdef F_HEART_BEATS
array_t *get_heart_beats() {
  std::vector<object_t *> result;
  result.reserve(heartbeats.size() + heartbeats_pending.size() +
                 (g_current_heartbeat_entry && g_current_heartbeat_entry->ob ? 1 : 0));

  bool display_hidden = true;
#ifdef F_SET_HIDE
  display_hidden = valid_hide(current_object);
#endif

  auto fn = [&](heart_beat_t &hb) {
    if (hb.ob) {
      if (hb.ob->flags & O_HIDDEN) {
        if (!display_hidden) {
          return;
        }
      }
      result.push_back(hb.ob);
    }
  };

  if (g_current_heartbeat_entry && g_current_heartbeat_entry->ob) {
    fn(*g_current_heartbeat_entry);
  }
  std::for_each(heartbeats.begin(), heartbeats.end(), fn);
  std::for_each(heartbeats_pending.begin(), heartbeats_pending.end(), fn);

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
  if (g_current_heartbeat_entry && g_current_heartbeat_entry->ob) {
    DEBUG_CHECK(!objset.insert(g_current_heartbeat_entry->ob).second,
                "Driver BUG: Duplicated/Missing heartbeats found");
  }
  for (auto &hb : heartbeats) {
    if (hb.ob) {
      DEBUG_CHECK(!objset.insert(hb.ob).second, "Driver BUG: Duplicated/Missing heartbeats found");
    }
  }
  for (auto &hb : heartbeats_pending) {
    if (hb.ob) {
      DEBUG_CHECK(!objset.insert(hb.ob).second, "Driver BUG: Duplicated/Missing heartbeats found");
    }
  }
}

void clear_heartbeats() {
  // TODO: instead of clearing everything blindly, should go through all objects with heartbeat flag
  // and delete corresponding heartbeats, thus exposing leftovers.
  heartbeats.clear();
  heartbeats_pending.clear();
  heartbeat_intervals.clear();
  g_current_heartbeat_entry = nullptr;
  g_current_heartbeat_obj = nullptr;
}
