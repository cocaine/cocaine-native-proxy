#include "cocaine/proxy/server.hpp"

namespace {

void
init() {
    // Block the deprecated signals.
    sigset_t signal;
    sigemptyset(&signal);
    sigaddset(&signal, SIGPIPE);
    ::sigprocmask(SIG_BLOCK, &signal, nullptr);
}

} // namespace

int
main(int argc, char **argv) {
    init();

    return ioremap::thevoid::run_server<cocaine::proxy::server_t>(argc, argv);
}
