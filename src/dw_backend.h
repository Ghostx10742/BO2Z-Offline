#pragma once

namespace bo2lan {

// Installs selective Winsock import hooks for the stock Zombies process and
// starts the process-local Demonware compatibility backend. Every non-DW host
// and socket continues to use the original Winsock implementation.
bool StartBackend();
bool RunBackendSelfTests();

}
