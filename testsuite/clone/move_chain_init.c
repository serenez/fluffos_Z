int init_calls;

void reset_state() {
    init_calls = 0;
}

int query_init_calls() {
    return init_calls;
}

void init() {
    init_calls++;
}

void move(object dest) {
    move_object(dest);
}
