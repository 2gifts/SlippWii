# ENet 1.3.13 (vendored)

Upstream: <https://github.com/lsalzman/enet>, tag `v1.3.13`. This is the same
version Slippi Dolphin (Ishiiruka) bundles for matchmaking and netplay. MIT
licence: see `LICENSE`.

## Contents

| File | Status |
| --- | --- |
| `callbacks.c`, `host.c`, `list.c`, `packet.c`, `peer.c`, `protocol.c` | upstream, unmodified |
| `include/enet/{callbacks,list,protocol,time,types,utility}.h` | upstream, unmodified |
| `include/enet/enet.h` | upstream, plus one change: it includes `enet/ios.h` when `ENET_IOS` is defined |
| `include/enet/ios.h`, `ios.c` | new: the IOS platform layer (replaces `unix.c`/`win32.c`) |

`compress.c` (the range coder) is left out because Slippi does not use it.
Line endings are LF, as upstream.

## IOS platform layer

`ios.c` implements ENet's platform API on top of `../so_socket.h`:

- There is one UDP socket. `enet_socket_bind` opens it on the requested port
  (a random port in 41000-50999 for port 0, as Slippi Dolphin picks).
  `enet_socket_destroy` leaves it open for the next host on the same port
  and only flushes unread datagrams; see `../so_socket.h` for why.
- Sends and receives never block. Sends are asynchronous IOS requests; when
  every send buffer is in flight, `enet_socket_send` reports "would block" and
  ENet's reliability layer resends as usual.
- `enet_socket_wait` and `enet_socketset_select` wait on the socket's wake
  event. The Slippi client services its host with a zero timeout and does
  its own waiting, because that event also carries wake-ups from the game.
- Call `enet_ios_initialize` instead of `enet_initialize`: it routes ENet's
  memory to the slippi_online arena and makes out-of-memory non-fatal.
