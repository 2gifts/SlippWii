/**
 @file  ios.h
 @brief ENet platform header for the Wii's IOS (Nintendont kernel)

 Not part of upstream ENet; see ../../README.md. The Starlet runs big-endian,
 so network byte order is the host order.
*/
#ifndef __ENET_IOS_H__
#define __ENET_IOS_H__

#include <stdlib.h>

typedef int ENetSocket;

#define ENET_SOCKET_NULL -1

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
/* Only the PC test build (slippi_online/tests/pc) is little-endian. */
#define ENET_HOST_TO_NET_16(value) ((enet_uint16) __builtin_bswap16 (value))
#define ENET_HOST_TO_NET_32(value) ((enet_uint32) __builtin_bswap32 (value))
#else
#define ENET_HOST_TO_NET_16(value) ((enet_uint16) (value))
#define ENET_HOST_TO_NET_32(value) ((enet_uint32) (value))
#endif

#define ENET_NET_TO_HOST_16(value) ENET_HOST_TO_NET_16 (value)
#define ENET_NET_TO_HOST_32(value) ENET_HOST_TO_NET_32 (value)

typedef struct
{
    void * data;
    size_t dataLength;
} ENetBuffer;

#define ENET_CALLBACK

#define ENET_API extern

/* The backend has one socket (slippi_online/so_socket.h), so a set is a
   bitmask of descriptors 0-31. */
typedef unsigned int ENetSocketSet;

#define ENET_SOCKETSET_EMPTY(sockset)          ((sockset) = 0)
#define ENET_SOCKETSET_ADD(sockset, socket)    ((sockset) |= 1u << (socket))
#define ENET_SOCKETSET_REMOVE(sockset, socket) ((sockset) &= ~(1u << (socket)))
#define ENET_SOCKETSET_CHECK(sockset, socket)  (((sockset) >> (socket)) & 1u)

/* Initializes ENet with the slippi_online arena allocator; use instead of
   enet_initialize. */
ENET_API int enet_ios_initialize (void);

#endif /* __ENET_IOS_H__ */
