/*
 * user.cc
 *
 *  Created on: Oct 16, 2014
 *      Author: sunyc
 */
#include "base/std.h"

#include "user.h"

#include <algorithm>   // for count_if, for_each, remove
#include <cstring>     // for memset
#include <functional>  // for function
#include <utility>
#include <vector>  // for vector

#include "interactive.h"  // for interactive_t->ob
#include "vm/vm.h"

// structure that holds all users
static std::vector<interactive_t *> all_users;

namespace {

int next_text_capacity(int current_capacity, int required_size) {
  int capacity = current_capacity > 0 ? current_capacity : INTERACTIVE_TEXT_INITIAL_CAPACITY;
  while (capacity < required_size && capacity < MAX_TEXT) {
    if (capacity > MAX_TEXT / 2) {
      capacity = MAX_TEXT;
    } else {
      capacity *= 2;
    }
  }
  return capacity;
}

}  // namespace

interactive_t *user_add() {
  auto *user = reinterpret_cast<interactive_t *>(
      DMALLOC(sizeof(interactive_t), TAG_INTERACTIVE, "new_conn_handler"));
  if (!user) {
    return nullptr;
  }
  memset(user, 0, sizeof(*user));
  if (!interactive_ensure_text_capacity(user, INTERACTIVE_TEXT_INITIAL_CAPACITY)) {
    FREE(user);
    return nullptr;
  }
  all_users.push_back(user);
  return user;
}

void user_del(interactive_t *user) {
  // remove it from global table.
  all_users.erase(std::remove(all_users.begin(), all_users.end(), user), all_users.end());
}

bool interactive_ensure_text_capacity(interactive_t *user, int required_size) {
  if (!user || required_size < 0 || required_size > MAX_TEXT) {
    return false;
  }

  if (user->text && required_size <= user->text_capacity) {
    return true;
  }

  int new_capacity = next_text_capacity(user->text_capacity, required_size);
  if (new_capacity < required_size || new_capacity > MAX_TEXT) {
    return false;
  }

  auto *new_text = reinterpret_cast<char *>(
      DREALLOC(user->text, new_capacity, TAG_INTERACTIVE, "interactive_text_resize"));
  if (!new_text) {
    return false;
  }

  if (new_capacity > user->text_capacity) {
    memset(new_text + user->text_capacity, 0, new_capacity - user->text_capacity);
  }

  user->text = new_text;
  user->text_capacity = new_capacity;
  return true;
}

void interactive_compact_text(interactive_t *user) {
  if (!user || !user->text) {
    return;
  }

  if (user->text_start > 0 && user->text_end > user->text_start) {
    memmove(user->text, user->text + user->text_start, user->text_end - user->text_start);
    user->text_end -= user->text_start;
    user->text_start = 0;
  } else if (user->text_start >= user->text_end) {
    interactive_reset_text(user);
    return;
  }

  if (user->text_end >= 0 && user->text_end < user->text_capacity) {
    user->text[user->text_end] = '\0';
  }
}

void interactive_reset_text(interactive_t *user) {
  if (!user) {
    return;
  }

  user->text_start = 0;
  user->text_end = 0;
  if (user->text && user->text_capacity > 0) {
    user->text[0] = '\0';
  }
}

void interactive_free_text(interactive_t *user) {
  if (!user) {
    return;
  }

  if (user->text) {
    FREE(user->text);
    user->text = nullptr;
  }
  user->text_capacity = 0;
  user->text_start = 0;
  user->text_end = 0;
}

// Get a copy of all users
const std::vector<interactive_t *> &users() { return all_users; }

// Count users
int users_num(bool include_hidden) {
  if (include_hidden) {
    return all_users.size();
  }
  return std::count_if(all_users.begin(), all_users.end(),
                       [](interactive_t *user) { return (user->ob->flags & O_HIDDEN) == 0; });
}

void users_foreach(std::function<void(interactive_t *)> func) {
  std::for_each(all_users.begin(), all_users.end(), std::move(func));
}
