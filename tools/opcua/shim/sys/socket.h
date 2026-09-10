/* Minimal <sys/socket.h> for the bare-metal open62541 cross build.
 *
 * open62541's deps/musl_inet_pton.h includes <sys/socket.h> "for AF_INET" on
 * every architecture that is not WIN32 or LWIP.  With UA_ARCHITECTURE=none
 * there is no socket layer at all — newlib has no such header — and the file
 * is compiled unconditionally, so the build stops there.
 *
 * AF_INET and AF_INET6 are the entire surface it uses (verified by grepping
 * musl_inet_pton.c: those two identifiers and nothing else).  Providing them
 * is not a workaround around a bug; supplying what the library expects from
 * the platform IS the port.  The values are the universal BSD ones, which
 * lwIP, newlib's networking-enabled variants and every other stack agree on.
 *
 * Deliberately NOT a general socket header: no sockaddr, no socket(), no
 * types the OPC-UA transport could accidentally start depending on.  This
 * server's transport is Arduino's Client API, and if a future change makes
 * something here look insufficient, the right answer is to check why sockets
 * are being reached for — not to grow this file.
 */

#ifndef OPCUA_BAREMETAL_SYS_SOCKET_SHIM_H
#define OPCUA_BAREMETAL_SYS_SOCKET_SHIM_H

#ifndef AF_INET
#define AF_INET 2
#endif

#ifndef AF_INET6
#define AF_INET6 10
#endif

#endif /* OPCUA_BAREMETAL_SYS_SOCKET_SHIM_H */
