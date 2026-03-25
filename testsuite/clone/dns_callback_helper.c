private mapping last_result = 0;

void clear_result() {
  last_result = 0;
}

void test_resolve_callback(mixed name, mixed addr, int key) {
  last_result = ([
    "name": name,
    "addr": addr,
    "key": key
  ]);
}

mapping query_result() {
  return last_result;
}
