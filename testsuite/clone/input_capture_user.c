private string last_input = 0;
private string *input_history = ({});
private int destroy_on_input = 0;

int process_input(string arg) {
  last_input = arg;
  input_history += ({ arg });
  if (destroy_on_input) {
    destruct(this_object());
  }
  return 1;
}

string query_last_input() { return last_input; }

string *query_input_history() { return input_history; }

void enable_destroy_on_input() { destroy_on_input = 1; }

void disable_destroy_on_input() { destroy_on_input = 0; }

void clear_inputs() {
  last_input = 0;
  input_history = ({});
  destroy_on_input = 0;
}
