private string last_input = 0;
private string *input_history = ({});

int process_input(string arg) {
  last_input = arg;
  input_history += ({ arg });
  return 1;
}

string query_last_input() { return last_input; }

string *query_input_history() { return input_history; }

void clear_inputs() {
  last_input = 0;
  input_history = ({});
}
