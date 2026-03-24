private string last_input = 0;

int process_input(string arg) {
  last_input = arg;
  return 1;
}

string query_last_input() { return last_input; }
