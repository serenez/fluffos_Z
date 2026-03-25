#include <globals.h>

private mapping last_gateway_login = ([]);
private mixed last_gateway_payload = 0;
private string last_disconnect_code = 0;
private string last_disconnect_text = 0;

void finish_gateway_logon(mixed data) {
    if (mapp(data)) {
        last_gateway_login = data;
    } else {
        last_gateway_login = ([ "raw": data ]);
    }
}

mapping query_gateway_login() { return last_gateway_login; }

mixed query_last_gateway_payload() { return last_gateway_payload; }

mixed query_gateway_session_snapshot() { return gateway_session_info(this_object()); }

string query_last_disconnect_code() { return last_disconnect_code; }

string query_last_disconnect_text() { return last_disconnect_text; }

void gateway_receive(mixed data) {
    last_gateway_payload = data;
}

void gateway_disconnected(string reason_code, string reason_text) {
    last_disconnect_code = reason_code;
    last_disconnect_text = reason_text;
}

void net_dead() {
    if (!last_disconnect_code) {
        last_disconnect_code = "net_dead";
    }
}
