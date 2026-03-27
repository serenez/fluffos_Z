int fired;

void create() {
  fired = 0;
}

void reset_state() {
  fired = 0;
}

void nop() {
  fired++;
}

int query_fired() {
  return fired;
}
