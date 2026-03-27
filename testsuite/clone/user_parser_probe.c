string *history = ({});
int accept_hits;
int reject_hits;

private string format_arg(mixed arg) {
    if (undefinedp(arg)) {
        return "<undefined>";
    }
    if (stringp(arg)) {
        return arg;
    }
    return sprintf("%O", arg);
}

private int record(string label, mixed arg, int result) {
    history += ({ label + ":" + format_arg(arg) });
    if (result) {
        accept_hits++;
    } else {
        reject_hits++;
    }
    return result;
}

void reset_state() {
    history = ({});
    accept_hits = 0;
    reject_hits = 0;
}

string *query_history() {
    return history + ({});
}

int query_accept_hits() {
    return accept_hits;
}

int query_reject_hits() {
    return reject_hits;
}

int record_default_zero(mixed arg) {
    return record("default", arg, 0);
}

int record_default_one(mixed arg) {
    return record("default", arg, 1);
}

int record_exact_zero(mixed arg) {
    return record("exact", arg, 0);
}

int record_exact_one(mixed arg) {
    return record("exact", arg, 1);
}

int record_short_zero(mixed arg) {
    return record("short", arg, 0);
}

int record_short_one(mixed arg) {
    return record("short", arg, 1);
}

int record_nospace_zero(mixed arg) {
    return record("nospace", arg, 0);
}

int record_nospace_one(mixed arg) {
    return record("nospace", arg, 1);
}

int accept(mixed arg) {
    accept_hits++;
    return 1;
}

int reject(mixed arg) {
    reject_hits++;
    return 0;
}
