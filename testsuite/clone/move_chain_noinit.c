int reset_calls;

void reset_state() {
    reset_calls = 0;
}

int query_init_calls() {
    return 0;
}

int query_reset_calls() {
    return reset_calls;
}

void reset() {
    reset_calls++;
}

void move(object dest) {
    move_object(dest);
}
