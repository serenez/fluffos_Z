#include <globals.h>

private mapping last_gateway_login = ([]);

void logon() {
    write("Normal login path reached.\n");
    write("This example object is mainly intended for gateway sessions.\n");
}

void gateway_logon(mixed data) {
    if (mapp(data)) {
        last_gateway_login = data;
    } else {
        last_gateway_login = ([ "raw": data ]);
    }

    write("Gateway session attached.\n");
    write(sprintf("gateway login data: %O\n", data));

    gateway_session_send(this_object(), ([
        "type": "welcome",
        "data": ([
            "message": "gateway session ready",
            "session": gateway_session_info(this_object()),
        ]),
    ]));
}

void gateway_receive(mixed data) {
    if (stringp(data)) {
        write("Gateway string payload: " + data + "\n");

        if (data == "ping") {
            gateway_session_send(this_object(), ([
                "type": "pong",
                "data": "pong",
            ]));
            return;
        }

        /*
         * The driver does not auto-inject gateway payloads into the
         * traditional command buffer. If your mudlib wants command-style
         * handling, do it explicitly here.
         */
        // command(data);
        return;
    }

    write(sprintf("Gateway structured payload: %O\n", data));
}

void gateway_disconnected(string reason_code, string reason_text) {
    last_gateway_login = ([]);
    write(sprintf("Gateway disconnected: %s (%s)\n", reason_code, reason_text));
}

void net_dead() {
    last_gateway_login = ([]);
}
