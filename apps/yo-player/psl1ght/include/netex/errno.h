#pragma once

/*
 * Compatibility definitions for PSL1GHT builds that provide net/net.h
 * but do not ship Sony's netex/errno.h wrapper.
 */
#include <net/net.h>

#ifndef sys_net_errno
#define sys_net_errno (*netErrnoLoc())
#endif

#ifndef SYS_NET_EAGAIN
#define SYS_NET_EAGAIN       35
#endif
#ifndef SYS_NET_EWOULDBLOCK
#define SYS_NET_EWOULDBLOCK  SYS_NET_EAGAIN
#endif
#ifndef SYS_NET_EINPROGRESS
#define SYS_NET_EINPROGRESS  36
#endif
#ifndef SYS_NET_EALREADY
#define SYS_NET_EALREADY     37
#endif
