private string last_input = 0;
private string *input_history = ({});
private int destroy_on_input = 0;
private int exec_on_input = 0;

int process_input(string arg) {
  last_input = arg;
  input_history += ({ arg });
  if (exec_on_input) {
    object next_user = new("/clone/input_capture_user");
    next_user->set_input_history(input_history + ({}));
    next_user->set_last_input(last_input);
    exec(next_user, this_object());
    destruct(this_object());
    return 1;
  }
  if (destroy_on_input) {
    destruct(this_object());
  }
  return 1;
}

string query_last_input() { return last_input; }

string *query_input_history() { return input_history; }

void enable_destroy_on_input() { destroy_on_input = 1; }

void disable_destroy_on_input() { destroy_on_input = 0; }

void enable_exec_on_input() { exec_on_input = 1; }

void disable_exec_on_input() { exec_on_input = 0; }

void set_last_input(string arg) { last_input = arg; }

void set_input_history(string *history) { input_history = history; }

void clear_inputs() {
  last_input = 0;
  input_history = ({});
  destroy_on_input = 0;
  exec_on_input = 0;
}
