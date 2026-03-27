private int input_count = 0;
private mixed last_input = 0;

int process_input(mixed arg) {
  input_count++;
  last_input = arg;
  return 1;
}

int query_input_count() { return input_count; }

mixed query_last_input() { return last_input; }

void reset_state() {
  input_count = 0;
  last_input = 0;
}
