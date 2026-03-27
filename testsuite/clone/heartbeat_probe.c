private int hb_count = 0;
private int action_tick = 0;
private int action_first = -1;
private int action_second = -1;
private object target_ob = 0;
private int target_action_tick = 0;
private int target_action_value = -1;

void reset_state() {
    hb_count = 0;
    action_tick = 0;
    action_first = -1;
    action_second = -1;
    target_ob = 0;
    target_action_tick = 0;
    target_action_value = -1;
    set_heart_beat(0);
}

void configure_action(int trigger_tick, int first, int second) {
    action_tick = trigger_tick;
    action_first = first;
    action_second = second;
}

void configure_target_action(object target, int trigger_tick, int value) {
    target_ob = target;
    target_action_tick = trigger_tick;
    target_action_value = value;
}

int query_hb_count() {
    return hb_count;
}

int query_hb_interval() {
    return query_heart_beat(this_object());
}

void set_probe_heartbeat(int value) {
    set_heart_beat(value);
}

void heart_beat() {
    hb_count++;

    if (action_tick && hb_count == action_tick) {
        if (action_first >= 0) {
            set_heart_beat(action_first);
        }
        if (action_second >= 0) {
            set_heart_beat(action_second);
        }
    }

    if (target_action_tick && hb_count == target_action_tick && objectp(target_ob)) {
        call_other(target_ob, "set_probe_heartbeat", target_action_value);
    }
}
