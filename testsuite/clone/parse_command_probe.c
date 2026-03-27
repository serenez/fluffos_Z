nosave string *id_list = ({ "token" });
nosave string *plural_id_list = ({ "tokens" });
nosave string *adjective_id_list = ({ "ancient", "bronze" });

private void rebuild_lists(int variant, int id_count, int adjective_count,
                           int include_shared_noun, int include_shared_adjectives) {
  int i;

  id_list = ({});
  plural_id_list = ({});
  adjective_id_list = ({});

  for (i = 0; i < id_count; i++) {
    string noun = sprintf("noun_%d_%d", variant, i);
    id_list += ({ noun });
    plural_id_list += ({ noun + "s" });
  }

  if (include_shared_noun) {
    id_list += ({ "token" });
    plural_id_list += ({ "tokens" });
  }

  for (i = 0; i < adjective_count; i++) {
    adjective_id_list += ({ sprintf("adj_%d_%d", variant, i) });
  }

  if (include_shared_adjectives) {
    adjective_id_list += ({ "ancient", "bronze" });
  }
}

void reset_state() {
  rebuild_lists(0, 0, 0, 1, 1);
}

void configure_lists(int variant, int id_count, int adjective_count,
                     int include_shared_noun, int include_shared_adjectives) {
  rebuild_lists(variant, id_count, adjective_count, include_shared_noun,
                include_shared_adjectives);
}

string *parse_command_id_list() {
  return id_list;
}

string *parse_command_plural_id_list() {
  return plural_id_list;
}

string *parse_command_adjective_id_list() {
  return adjective_id_list;
}
