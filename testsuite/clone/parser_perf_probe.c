nosave string noun_value = "token";
nosave int living_flag = 0;
nosave int inventory_visible_flag = 1;
nosave int inventory_accessible_flag = 1;

void create() {
  parse_init();
}

void reset_state() {
  noun_value = "token";
  living_flag = 0;
  inventory_visible_flag = 1;
  inventory_accessible_flag = 1;
  parse_refresh();
}

void configure_target(string noun, int living, int visible, int accessible) {
  noun_value = noun;
  living_flag = living;
  inventory_visible_flag = visible;
  inventory_accessible_flag = accessible;
  parse_refresh();
}

string *parse_command_id_list() {
  return ({ noun_value });
}

string *parse_command_plural_id_list() {
  return ({ noun_value + "s" });
}

int is_living() {
  return living_flag;
}

int inventory_visible() {
  return inventory_visible_flag;
}

int inventory_accessible() {
  return inventory_accessible_flag;
}
