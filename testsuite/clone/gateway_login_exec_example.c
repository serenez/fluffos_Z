#include <globals.h>

void logon() {
    write("Normal login path reached.\n");
}

void gateway_logon(mixed data) {
    object user = new("/clone/gateway_exec_user");
    exec(user, this_object());
    user->finish_gateway_logon(data);
    destruct(this_object());
}
