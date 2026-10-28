/**
 @file  ios.c
 @brief ENet platform functions for the Wii's IOS (Nintendont kernel)

 Not part of upstream ENet; see README.md. Sockets map onto the single
 non-blocking UDP socket of slippi_online/so_socket.h, so a host never waits
 on IOS while servicing; time, randomness and memory come from the other
 slippi_online modules.
*/
#define ENET_BUILDING_LIB 1
#include <string.h>
#include "enet/enet.h"
#include "../so_alloc.h"
#include "../so_platform.h"
#include "../so_socket.h"
#include "../so_util.h"

#define IOS_SOCKET 0

static enet_uint32 timeBase;
static int socketCreated;

static void *
ios_malloc (size_t size)
{
    SO_STEP ("enet malloc");
    return so_malloc ((u32) size);
}

static void
ios_no_memory (void)
{
    /* ENet copes with a failed allocation; the default handler would abort. */
    so_log ("enet: out of memory");
}

int
enet_ios_initialize (void)
{
    ENetCallbacks callbacks;

    callbacks.malloc = ios_malloc;
    callbacks.free = so_free;
    callbacks.no_memory = ios_no_memory;

    return enet_initialize_with_callbacks (ENET_VERSION, & callbacks);
}

/* Referenced by the default callbacks in callbacks.c, which
   enet_ios_initialize replaces; the kernel has no process to abort. */
void
abort (void)
{
    so_log ("enet: abort");
    for (;;)
      so_sleep_us (1000000);
}

int
enet_initialize (void)
{
    return 0;
}

void
enet_deinitialize (void)
{
}

enet_uint32
enet_host_random_seed (void)
{
    return so_random ();
}

enet_uint32
enet_time_get (void)
{
    return so_time_ms () - timeBase;
}

void
enet_time_set (enet_uint32 newTimeBase)
{
    timeBase = so_time_ms () - newTimeBase;
}

int
enet_address_set_host (ENetAddress * address, const char * name)
{
    u32 host;

    if (so_resolve (name, & host) < 0)
      return -1;

    address -> host = host;

    return 0;
}

int
enet_address_get_host_ip (const ENetAddress * address, char * name, size_t nameLength)
{
    char text [16];

    so_format_ipv4 (address -> host, text, sizeof (text));

    if (strlen (text) >= nameLength)
      return -1;

    memcpy (name, text, strlen (text) + 1);

    return 0;
}

int
enet_address_get_host (const ENetAddress * address, char * name, size_t nameLength)
{
    /* IOS has no reverse lookup; ENet falls back to the address too. */
    return enet_address_get_host_ip (address, name, nameLength);
}

ENetSocket
enet_socket_create (ENetSocketType type)
{
    SO_STEP ("enet socket");
    if (type != ENET_SOCKET_TYPE_DATAGRAM || socketCreated)
      return ENET_SOCKET_NULL;

    socketCreated = 1;

    return IOS_SOCKET;
}

static u16
ephemeral_port (void)
{
    /* The range Slippi Dolphin picks its own port from. */
    return (u16) (41000 + so_random () % 10000);
}

int
enet_socket_bind (ENetSocket socket, const ENetAddress * address)
{
    u16 port = address != NULL ? address -> port : 0;

    SO_STEP ("enet bind");
    if (socket != IOS_SOCKET)
      return -1;

    /* IOS cannot report the port it picks for 0, so pick one here. */
    if (port == 0)
      port = ephemeral_port ();

    return so_socket_open (port) < 0 ? -1 : 0;
}

int
enet_socket_get_address (ENetSocket socket, ENetAddress * address)
{
    SO_STEP ("enet get address");
    if (socket != IOS_SOCKET || ! so_socket_is_open ())
      return -1;

    address -> host = so_local_ip ();
    address -> port = so_socket_port ();

    return 0;
}

int
enet_socket_listen (ENetSocket socket, int backlog)
{
    return -1;
}

int
enet_socket_set_option (ENetSocket socket, ENetSocketOption option, int value)
{
    SO_STEP ("enet option");
    /* The socket is always non-blocking for ENet, and IOS keeps its own
       buffer sizes. */
    return socket == IOS_SOCKET ? 0 : -1;
}

int
enet_socket_get_option (ENetSocket socket, ENetSocketOption option, int * value)
{
    if (socket != IOS_SOCKET)
      return -1;

    * value = 0;

    return 0;
}

int
enet_socket_connect (ENetSocket socket, const ENetAddress * address)
{
    return -1;
}

ENetSocket
enet_socket_accept (ENetSocket socket, ENetAddress * address)
{
    return ENET_SOCKET_NULL;
}

int
enet_socket_shutdown (ENetSocket socket, ENetSocketShutdown how)
{
    return -1;
}

void
enet_socket_destroy (ENetSocket socket)
{
    if (socket != IOS_SOCKET || ! socketCreated)
      return;

    /* The IOS socket stays open for the next host on the same port (see
       so_socket.h); only drop what this host left unread. */
    so_socket_flush ();
    socketCreated = 0;
}

int
enet_socket_send (ENetSocket socket,
                  const ENetAddress * address,
                  const ENetBuffer * buffers,
                  size_t bufferCount)
{
    SoSlice slices [ENET_BUFFER_MAXIMUM];
    size_t i;
    int sent;

    SO_STEP ("enet send");
    if (socket != IOS_SOCKET || address == NULL || bufferCount > ENET_BUFFER_MAXIMUM)
      return -1;

    if (! so_socket_is_open () && enet_socket_bind (socket, NULL) < 0)
      return -1;

    for (i = 0; i < bufferCount; ++ i)
    {
        slices [i].data = buffers [i].data;
        slices [i].len = (u32) buffers [i].dataLength;
    }

    sent = so_socket_sendv (address -> host, address -> port, slices, (int) bufferCount);

    /* -2: every send buffer is in flight, which ENet treats as a full
       socket buffer (would block). */
    if (sent == -2)
      return 0;

    return sent;
}

int
enet_socket_receive (ENetSocket socket,
                     ENetAddress * address,
                     ENetBuffer * buffers,
                     size_t bufferCount)
{
    u32 host;
    u16 port;
    int length;

    /* ENet receives into one buffer of ENET_PROTOCOL_MAXIMUM_MTU bytes. */
    if (socket != IOS_SOCKET || bufferCount != 1)
      return -1;

    length = so_socket_receive (& host, & port, buffers [0].data, (u32) buffers [0].dataLength, NULL);

    if (length > 0 && address != NULL)
    {
        address -> host = host;
        address -> port = port;
    }

    return length;
}

int
enet_socketset_select (ENetSocket maxSocket, ENetSocketSet * readSet, ENetSocketSet * writeSet, enet_uint32 timeout)
{
    int ready = 0;

    if (readSet != NULL && ENET_SOCKETSET_CHECK (* readSet, IOS_SOCKET))
    {
        if (so_socket_wait (timeout * 1000))
          ready = 1;
        else
          ENET_SOCKETSET_EMPTY (* readSet);
    }

    /* Sends never block. */
    if (writeSet != NULL && ENET_SOCKETSET_CHECK (* writeSet, IOS_SOCKET))
      ready = 1;

    return ready;
}

int
enet_socket_wait (ENetSocket socket, enet_uint32 * condition, enet_uint32 timeout)
{
    enet_uint32 wanted = * condition;

    if (socket != IOS_SOCKET)
      return -1;

    * condition = ENET_SOCKET_WAIT_NONE;

    if (wanted & ENET_SOCKET_WAIT_SEND)
      * condition |= ENET_SOCKET_WAIT_SEND;
    else if ((wanted & ENET_SOCKET_WAIT_RECEIVE) && so_socket_wait (timeout * 1000))
      * condition |= ENET_SOCKET_WAIT_RECEIVE;

    return 0;
}
