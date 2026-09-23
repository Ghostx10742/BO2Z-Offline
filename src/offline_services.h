#pragma once
#include "offline_protocol.h"
#include <winsock2.h>
#include <memory>
namespace bo2lan::offline {
enum class ServerKind { Auth, Lobby };
void SetServiceWakeCallback(void (*)());
class LocalServer {
public:
 explicit LocalServer(ServerKind,bool worker=false);
 ~LocalServer();
 int Send(const char*,int);
 int Receive(char*,int,int flags=0,bool blocking=false);
 bool Readable() const;
 std::size_t Available() const;
 bool Ready() const;
 bool Failed() const;
 void Close();
 void Dump(SOCKET) const;
private:
 class Impl;
 std::unique_ptr<Impl> impl_;
};
}
