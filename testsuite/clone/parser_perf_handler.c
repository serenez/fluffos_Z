nosave int remote_livings_enabled = 0;
nosave object last_target = 0;

void create() {
  parse_init();
  parse_add_rule("inspect", "OBJ");
}

void reset_state() {
  last_target = 0;
}

void set_remote_livings_enabled(int enabled) {
  remote_livings_enabled = enabled;
  parse_refresh();
}

int livings_are_remote() {
  return remote_livings_enabled;
}

int run_parse_input(string input, object *env) {
  last_target = 0;
  return parse_sentence(input, 0, env);
}

object query_last_target() {
  return last_target;
}

int can_inspect_obj() {
  return 1;
}

int direct_inspect_obj(object ob) {
  return objectp(ob);
}

int do_inspect_obj(object ob) {
  last_target = ob;
  return 1;
}
