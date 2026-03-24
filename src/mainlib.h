#include <string>

// Init everything driver needs.
struct event_base* init_main(std::string_view config_file, bool require_backend = true);
void setup_signal_handlers();
std::string get_argument(unsigned int pos, int argc, char** argv);
