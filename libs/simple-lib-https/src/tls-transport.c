// tls-transport - BearSSL HTTPS plumbing over a raw BSD socket (see tls-transport.h).

#include "tls-transport.h"
#include "bearssl.h"
#include "dbg.h"
#include "thread.h"   // resolver worker and locks

#include <string.h>
#include <stdio.h>
#include <stdlib.h>   // strtoull, atoi

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <netex/errno.h>
#include <net/poll.h>
#include <sys/timer.h>
#include <sys/sys_time.h>     // sys_time_get_current_time (wall clock for cert-date validation)
#include <sys/random_number.h>
#include <netex/net.h>

// the bundled root CAs (Google Trust Services + Let's Encrypt), generated in trust-anchors.c
extern const br_x509_trust_anchor TLS_TRUST_ANCHORS[];
extern const size_t TLS_TRUST_ANCHORS_COUNT;

#define TLS_PORT          443
#define SOCKET_TIMEOUT    5        // seconds; bounds stalled socket I/O and teardown latency
#define DNS_TIMEOUT_MS    5000
#define CONNECT_TIMEOUT_MS 5000
#define SOCKET_POLL_MS      20
#define TLS_HEADER_TIMEOUT_MS 10000
#define TLS_CHUNK_LINE_MAX   128
#define DAYS_0AD_TO_EPOCH 719528   // days from 0000-01-01 to 1970-01-01 (BearSSL's day count is from 0 AD)
#define SECONDS_PER_DAY   86400     // split the unix clock into BearSSL's (days, seconds-of-day) time
#define HEAD_MAX          8192     // response header block cap; larger is treated as a malformed response

struct TlsConn {
   int                    socketHandle;
   br_ssl_client_context  client;
   br_x509_minimal_context x509;
   br_sslio_context       io;
   int64_t                remaining;     // body bytes left per Content-Length; -1 when close-delimited
   int                    chunked;       // body arrives in chunks, each with its own size line, instead
   int64_t                chunkLeft;     // bytes left in the chunk being served
   int                    chunkedEnd;    // the zero-sized chunk that ends a chunked body has been read
   const unsigned char   *leftover;      // body bytes read while parsing the head (points into head)
   int                    leftoverLen;
   char                   host[256];     // remembered for connection reuse (pool key)
   int                    keepAlive;     // this request asked the server to keep the connection open
   int                    poolAllowed;   // explicit safety gate; experimental LIVE requests stay one-shot
   char                   statusLine[128];
   unsigned char          head[HEAD_MAX];
   unsigned char          ioBuffer[BR_SSL_BUFSIZE_BIDI];
};

// section: socket transport

// gethostbyname returns a pointer to shared static storage, so resolve+copy is serialised: several worker
// threads (the thumbnail fetchers) open connections at once. Created once by initTlsResolver at startup.
static sys_lwmutex_t resolveLock;
static int           resolveLockReady;
static sys_lwmutex_t resolverJobLock;
static int           resolverJobLockReady;
static volatile int  resolverJobState; // 0 idle, 1 resolving, 2 complete, 3 caller timed out
static char          resolverJobHost[256];
static struct in_addr resolverJobAddress;
static int           resolverJobResult;
static sys_lwmutex_t socketLock;
static int           socketLockReady;
static volatile int  requestsCancelled;
#define ACTIVE_SOCKET_MAX 64
static int activeSockets[ACTIVE_SOCKET_MAX];
static int activeSocketCount;
static unsigned long socketCreatedCount;
static unsigned long socketClosedCount;
static unsigned long socketCloseFailedCount;
static unsigned long socketOpenFailedCount;
static int activeSocketHighWater;
static TlsConn *activeConnections[ACTIVE_SOCKET_MAX];
static int activeConnectionCount;
static unsigned long connectionCreatedCount;
static unsigned long connectionClosedCount;
static unsigned long connectionCloseInvalidCount;

static int getActiveSocketCount(void)
{
   if (!socketLockReady) return 0;
   lock(&socketLock);
   int count = activeSocketCount;
   unlock(&socketLock);
   return count;
}

void initTlsResolver(void)
{
   if (!resolveLockReady) resolveLockReady = createLock(&resolveLock) == 0;
   if (!resolverJobLockReady) resolverJobLockReady = createLock(&resolverJobLock) == 0;
   if (!socketLockReady) socketLockReady = createLock(&socketLock) == 0;
}

static void lockResolve(void)   { if (resolveLockReady) lock(&resolveLock); }
static void unlockResolve(void) { if (resolveLockReady) unlock(&resolveLock); }

// DNS lookup runs on one detached worker because gethostbyname itself has no cancellation or timeout
// interface. The caller waits at most DNS_TIMEOUT_MS. If the resolver stalls, state 3 prevents spawning
// more workers; the orphaned worker returns the slot to idle only if/when gethostbyname eventually returns.
static void resolverWorker(uint64_t arg)
{
   (void)arg;
   char host[sizeof resolverJobHost];
   struct in_addr address;
   int result = 0;
   lock(&resolverJobLock);
   if (resolverJobState != 1) {
      if (resolverJobState == 3) resolverJobState = 0;
      unlock(&resolverJobLock);
      exitThread();
   }
   memcpy(host, resolverJobHost, sizeof host);
   host[sizeof host - 1] = '\0';
   unlock(&resolverJobLock);

   lockResolve();
   struct hostent *resolved = gethostbyname(host);
   if (resolved && resolved->h_addr_list && resolved->h_addr_list[0]) {
      memcpy(&address, resolved->h_addr_list[0], sizeof address);
      result = 1;
   }
   unlockResolve();

   lock(&resolverJobLock);
   resolverJobResult = result;
   if (result) resolverJobAddress = address;
   __sync_synchronize();
   if (resolverJobState == 3) resolverJobState = 0;
   else resolverJobState = 2;
   unlock(&resolverJobLock);
   exitThread();
}

static int resolveHostWithTimeout(const char *host, struct in_addr *address)
{
   if (!resolveLockReady || !resolverJobLockReady) return -1;
   uint64_t deadline = sys_time_get_system_time() + (uint64_t)DNS_TIMEOUT_MS * 1000ull;
   for (;;) {
      if (requestsCancelled) {
         lock(&resolverJobLock);
         if (resolverJobState == 1) resolverJobState = 3;
         else if (resolverJobState == 2) resolverJobState = 0;
         unlock(&resolverJobLock);
         return -1;
      }
      lock(&resolverJobLock);
      int state = resolverJobState;
      if (state == 2 && strcmp(resolverJobHost, host) == 0) {
         int result = resolverJobResult;
         if (result) *address = resolverJobAddress;
         resolverJobState = 0;
         unlock(&resolverJobLock);
         return result ? 0 : -1;
      }
      if (state == 0) {
         resolverJobState = 1;
         strncpy(resolverJobHost, host, sizeof resolverJobHost - 1);
         resolverJobHost[sizeof resolverJobHost - 1] = '\0';
         resolverJobResult = 0;
         unlock(&resolverJobLock);
         sys_ppu_thread_t tid;
         if (spawnThread(&tid, resolverWorker, 0, THREAD_PRIORITY_DEFAULT,
                         THREAD_STACK_SIZE_64KB, "tls-dns") != 0) {
            lock(&resolverJobLock);
            if (resolverJobState == 1 && strcmp(resolverJobHost, host) == 0) resolverJobState = 0;
            unlock(&resolverJobLock);
            return -1;
         }
         continue;
      }
      if (requestsCancelled) {
         if (state == 1) resolverJobState = 3;
         else if (state == 2) resolverJobState = 0;
         unlock(&resolverJobLock);
         return -1;
      }
      if (state == 1 && strcmp(resolverJobHost, host) == 0 &&
          sys_time_get_system_time() >= deadline) {
         resolverJobState = 3;
         unlock(&resolverJobLock);
         logWarn("[tls-diag] DNS lookup timed out host=%s timeoutMs=%d\n", host, DNS_TIMEOUT_MS);
         return -1;
      }
      unlock(&resolverJobLock);
      if (sys_time_get_system_time() >= deadline) {
         logWarn("[tls-diag] DNS slot unavailable host=%s timeoutMs=%d\n", host, DNS_TIMEOUT_MS);
         return -1;
      }
      sleepMs(10);
   }
}

static int waitSocketReady(int handle, int writing, unsigned timeoutMs, short *readyEvents)
{
   if (readyEvents) *readyEvents = 0;
   if (handle < 0) return -1;

   const uint64_t deadline = sys_time_get_system_time() + (uint64_t)timeoutMs * 1000ull;
   struct pollfd descriptor;
   descriptor.fd = handle;
   descriptor.events = writing ? POLLOUT : POLLIN;
   descriptor.revents = 0;

   while (!requestsCancelled) {
      uint64_t now = sys_time_get_system_time();
      if (now >= deadline) return 0;

      uint64_t remainingUs = deadline - now;
      unsigned remainingMs = (unsigned)((remainingUs + 999ull) / 1000ull);
      int pollTimeout = (int)(remainingMs > SOCKET_POLL_MS ? SOCKET_POLL_MS : remainingMs);
      if (pollTimeout < 1) pollTimeout = 1;

      descriptor.revents = 0;
      int polled = netPoll(&descriptor, 1, pollTimeout);
      if (polled < 0) {
         logWarn("[tls-diag] netPoll failed handle=%d errno=%d\n", handle, sys_net_errno);
         return -1;
      }
      if (polled == 0) continue;

      if (readyEvents) *readyEvents = descriptor.revents;
      if (descriptor.revents & POLLNVAL) {
         logWarn("[tls-diag] netPoll invalid socket handle=%d revents=0x%x\n",
                 handle, (unsigned)descriptor.revents);
         return -1;
      }
      if (descriptor.revents & (descriptor.events | POLLERR | POLLHUP)) return 1;
   }

   return -1;
}

static int registerSocket(int handle)
{
   if (!socketLockReady) return 0;
   lock(&socketLock);
   int accepted = !requestsCancelled && activeSocketCount < ACTIVE_SOCKET_MAX;
   if (accepted) {
      activeSockets[activeSocketCount++] = handle;
      socketCreatedCount++;
      if (activeSocketCount > activeSocketHighWater) activeSocketHighWater = activeSocketCount;
   }
   unlock(&socketLock);
   return accepted;
}

static int unregisterSocket(int handle)
{
   if (!socketLockReady) return 0;
   int found = 0;
   lock(&socketLock);
   for (int i = 0; i < activeSocketCount; i++) {
      if (activeSockets[i] != handle) continue;
      activeSockets[i] = activeSockets[--activeSocketCount];
      found = 1;
      break;
   }
   unlock(&socketLock);
   return found;
}

static int isSocketRegistered(int handle)
{
   if (!socketLockReady) return 0;
   int found = 0;
   lock(&socketLock);
   for (int i = 0; i < activeSocketCount; i++) {
      if (activeSockets[i] == handle) { found = 1; break; }
   }
   unlock(&socketLock);
   return found;
}

static void recordSocketOpenFailure(void)
{
   if (!socketLockReady) return;
   lock(&socketLock);
   socketOpenFailedCount++;
   unlock(&socketLock);
}

static int registerTlsConnection(TlsConn *conn)
{
   if (!socketLockReady) return 0;
   lock(&socketLock);
   int accepted = activeConnectionCount < ACTIVE_SOCKET_MAX;
   if (accepted) {
      activeConnections[activeConnectionCount++] = conn;
      connectionCreatedCount++;
   }
   unlock(&socketLock);
   return accepted;
}

static int unregisterTlsConnection(TlsConn *conn)
{
   if (!socketLockReady) return 0;
   int found = 0;
   lock(&socketLock);
   for (int i = 0; i < activeConnectionCount; i++) {
      if (activeConnections[i] != conn) continue;
      activeConnections[i] = activeConnections[--activeConnectionCount];
      connectionClosedCount++;
      found = 1;
      break;
   }
   if (!found) connectionCloseInvalidCount++;
   unlock(&socketLock);
   return found;
}

void setTlsRequestsCancelled(int cancelled)
{
   if (!socketLockReady) { requestsCancelled = cancelled != 0; return; }
   lock(&socketLock);
   requestsCancelled = cancelled != 0;
   if (requestsCancelled)
      for (int i = 0; i < activeSocketCount; i++) netShutdown(activeSockets[i], SHUT_RDWR);
   unlock(&socketLock);
}

static int readSocket(int handle, void *buffer, int length)
{
   for (unsigned waited = 0; waited < SOCKET_TIMEOUT * 1000u && !requestsCancelled; waited += SOCKET_POLL_MS) {
      int got = netRecv(handle, buffer, length, 0);
      if (got > 0) return got;
      if (got == 0) return -1;
      if (sys_net_errno != SYS_NET_EWOULDBLOCK && sys_net_errno != SYS_NET_EAGAIN) return -1;
      int ready = waitSocketReady(handle, 0, SOCKET_POLL_MS, NULL);
      if (ready < 0) return -1;
   }
   return -1;
}

static int writeSocket(int handle, const void *data, int length)
{
   for (unsigned waited = 0; waited < SOCKET_TIMEOUT * 1000u && !requestsCancelled; waited += SOCKET_POLL_MS) {
      int sent = netSend(handle, data, length, 0);
      if (sent > 0) return sent;
      if (sent == 0) return -1;
      if (sys_net_errno != SYS_NET_EWOULDBLOCK && sys_net_errno != SYS_NET_EAGAIN) return -1;
      int ready = waitSocketReady(handle, 1, SOCKET_POLL_MS, NULL);
      if (ready < 0) return -1;
   }
   return -1;
}

static void closeSocket(int handle)
{
   // A socket handle may be reused by LV2 after close. Never issue a second close for an
   // untracked handle: it could otherwise close an unrelated socket that reused the number.
   int wasRegistered = isSocketRegistered(handle);
   if (!wasRegistered) {
      logError("[tls-audit] duplicate-or-untracked socket close handle=%d ignored\n", handle);
      return;
   }
   int closeRc = netClose(handle);
   int closeError = sys_net_errno;
   if (closeRc == 0) unregisterSocket(handle);
   int activeAfter;
   unsigned long closed, closeFailed, created, openFailed;
   int highWater;
   if (socketLockReady) {
      lock(&socketLock);
      if (closeRc == 0) socketClosedCount++;
      else socketCloseFailedCount++;
      activeAfter = activeSocketCount;
      closed = socketClosedCount;
      closeFailed = socketCloseFailedCount;
      created = socketCreatedCount;
      openFailed = socketOpenFailedCount;
      highWater = activeSocketHighWater;
      unlock(&socketLock);
   } else {
      activeAfter = 0;
      closed = closeFailed = created = openFailed = 0;
      highWater = 0;
   }
   logInfo("[tls-audit] socket-close handle=%d tracked=%d rc=%d errno=%d active=%d created=%lu closed=%lu close-failed=%lu open-failed=%lu high-water=%d\n",
           handle, wasRegistered, closeRc, closeError, activeAfter, created, closed,
           closeFailed, openFailed, highWater);
}

// resolve host and open a TCP connection to port 443. returns the socket, or -1.
static int connectHost(const char *host, int port)
{
   if (requestsCancelled) return -1;
   struct sockaddr_in address;
   memset(&address, 0, sizeof address);
   address.sin_family = AF_INET;
   address.sin_port = htons((uint16_t)port);

   // gethostbyname is isolated on a detached worker so an unresponsive resolver cannot block a player
   // or teardown thread. The caller has a hard wait limit and only one resolver worker may exist.
   uint64_t dnsStartUs = sys_time_get_system_time();
   int resolvedOk = resolveHostWithTimeout(host, &address.sin_addr) == 0;
   uint64_t dnsEndUs = sys_time_get_system_time();
   if (!resolvedOk) {
      logError("[tls-connect-timing] host=%s dnsMs=%llu result=failed\n", host,
               (unsigned long long)((dnsEndUs-dnsStartUs)/1000ull));
      logError("[tls-diag] dns failed host=%s cancelled=%d\n", host, requestsCancelled);
      return -1;
   }
   if (requestsCancelled) return -1;

   int socketHandle = netSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
   if (socketHandle < 0) {
      int socketError = sys_net_errno;
      recordSocketOpenFailure();
      logError("[tls-audit] socket-open-failed host=%s errno=%d active=%d\n",
               host, socketError, getActiveSocketCount());
      return -1;
   }
   if (!registerSocket(socketHandle)) {
      logWarn("[tls-diag] socket rejected host=%s handle=%d cancelled=%d active=%d active-limit=%d\n",
              host, socketHandle, requestsCancelled, getActiveSocketCount(), ACTIVE_SOCKET_MAX);
      int closeRc = netClose(socketHandle);
      int closeError = sys_net_errno;
      logInfo("[tls-audit] rejected-socket-close handle=%d rc=%d errno=%d active=%d\n",
              socketHandle, closeRc, closeError, getActiveSocketCount());
      return -1;
   }
   logInfo("[tls-diag] socket registered host=%s handle=%d active=%d\n",
           host, socketHandle, getActiveSocketCount());

   int nonblocking = 1;
   if (netSetSockOpt(socketHandle, SOL_SOCKET, SO_NBIO, &nonblocking, sizeof nonblocking) < 0) {
      logError("[tls] cannot enable nonblocking connect host=%s errno=%d\n", host, sys_net_errno);
      closeSocket(socketHandle);
      return -1;
   }

   uint64_t tcpStartUs = sys_time_get_system_time();
   int connectRc = netConnect(socketHandle, (struct sockaddr *)&address, sizeof address);
   int connectError = connectRc < 0 ? sys_net_errno : 0;
   if (connectRc < 0 && connectError != SYS_NET_EINPROGRESS && connectError != SYS_NET_EALREADY) {
      uint64_t tcpEndUs = sys_time_get_system_time();
      logError("[tls-connect-timing] host=%s dnsMs=%llu tcpMs=%llu result=failed errno=%d\n",
               host, (unsigned long long)((dnsEndUs-dnsStartUs)/1000ull),
               (unsigned long long)((tcpEndUs-tcpStartUs)/1000ull), connectError);
      closeSocket(socketHandle);
      return -1;
   }
   int connected = connectRc == 0;
   for (unsigned waited = 0; !connected && waited < CONNECT_TIMEOUT_MS && !requestsCancelled; waited += SOCKET_POLL_MS) {
      short readyEvents = 0;
      int ready = waitSocketReady(socketHandle, 1, SOCKET_POLL_MS, &readyEvents);
      if (ready < 0) break;
      if (ready > 0) {
         int socketError = 0;
         socklen_t errorLength = sizeof socketError;
         if (netGetSockOpt(socketHandle, SOL_SOCKET, SO_ERROR, &socketError, &errorLength) < 0) {
            connectError = sys_net_errno ? sys_net_errno : NET_EIO;
            logError("[tls] SO_ERROR query failed host=%s handle=%d errno=%d\n",
                     host, socketHandle, connectError);
            break;
         }
         if (errorLength < sizeof socketError) {
            connectError = NET_EIO;
            logError("[tls] SO_ERROR returned short value host=%s handle=%d length=%u\n",
                     host, socketHandle, (unsigned)errorLength);
            break;
         }
         if (socketError != 0) {
            connectError = socketError;
            break;
         }
         if (readyEvents & POLLOUT) connected = 1;
         else {
            connectError = NET_ECONNRESET;
            logError("[tls] socket became exceptional before connect completed host=%s handle=%d revents=0x%x\n",
                     host, socketHandle, (unsigned)readyEvents);
            break;
         }
      }
   }
   if (!connected) {
      uint64_t tcpEndUs = sys_time_get_system_time();
      if (!requestsCancelled)
         logError("[tls-connect-timing] host=%s dnsMs=%llu tcpMs=%llu result=failed errno=%d timeoutMs=%d\n",
                  host, (unsigned long long)((dnsEndUs-dnsStartUs)/1000ull),
                  (unsigned long long)((tcpEndUs-tcpStartUs)/1000ull), connectError, CONNECT_TIMEOUT_MS);
      closeSocket(socketHandle);
      return -1;
   }
   uint64_t tcpEndUs = sys_time_get_system_time();
   logInfo("[tls-connect-timing] host=%s dnsMs=%llu tcpMs=%llu result=connected\n",
           host, (unsigned long long)((dnsEndUs-dnsStartUs)/1000ull),
           (unsigned long long)((tcpEndUs-tcpStartUs)/1000ull));
   logInfo("[tls-diag] tcp connected host=%s handle=%d\n", host, socketHandle);
   return socketHandle;
}

// section: the channel under TLS

static const TlsChannel socketChannel = { connectHost, readSocket, writeSocket, closeSocket };
static const TlsChannel *boundChannel;

void bindTlsChannel(const TlsChannel *channel) { boundChannel = channel; }

static const TlsChannel *getTlsChannel(void) { return boundChannel ? boundChannel : &socketChannel; }

static int readChannel(void *context, unsigned char *buffer, size_t length)
{
   return getTlsChannel()->read(*(int *)context, buffer, (int)length);
}

static int writeChannel(void *context, const unsigned char *data, size_t length)
{
   return getTlsChannel()->write(*(int *)context, data, (int)length);
}

// section: connection setup

TlsConn *openTlsConn(const char *host)
{
   int socketHandle = getTlsChannel()->open(host, TLS_PORT);
   if (socketHandle < 0) return NULL;

   TlsConn *conn = calloc(1, sizeof *conn);
   if (!conn) {
      logError("[tls-diag] TlsConn allocation failed host=%s handle=%d\n", host, socketHandle);
      getTlsChannel()->close(socketHandle);
      return NULL;
   }
   conn->socketHandle = socketHandle;
   if (!registerTlsConnection(conn)) {
      logError("[tls-audit] connection registry full conn=%p handle=%d\n", conn, socketHandle);
      getTlsChannel()->close(socketHandle);
      free(conn);
      return NULL;
   }
   conn->remaining = -1;
   conn->leftover = NULL;
   conn->leftoverLen = 0;
   conn->keepAlive = 0;
   conn->poolAllowed = 1;
   conn->statusLine[0] = '\0';
   strncpy(conn->host, host, sizeof conn->host - 1);
   conn->host[sizeof conn->host - 1] = '\0';

   br_ssl_client_init_full(&conn->client, &conn->x509, TLS_TRUST_ANCHORS, TLS_TRUST_ANCHORS_COUNT);

   // BearSSL has no built-in clock, so hand the X.509 engine the current UTC time; without it the cert's
   // notBefore/notAfter can't be checked and validation fails with BR_ERR_X509_TIME_UNKNOWN.
   sys_time_sec_t nowSeconds = 0;
   sys_time_nsec_t nowNanoseconds = 0;
   sys_time_get_current_time(&nowSeconds, &nowNanoseconds);
   br_x509_minimal_set_time(&conn->x509, (uint32_t)(nowSeconds / SECONDS_PER_DAY) + DAYS_0AD_TO_EPOCH, (uint32_t)(nowSeconds % SECONDS_PER_DAY));

   unsigned char seed[32];
   if (sys_get_random_number(seed, sizeof seed) != 0) { logError("[tls] no entropy\n"); closeTlsConn(conn); return NULL; }
   br_ssl_engine_inject_entropy(&conn->client.eng, seed, sizeof seed);

   br_ssl_engine_set_buffer(&conn->client.eng, conn->ioBuffer, sizeof conn->ioBuffer, 1);
   br_ssl_client_reset(&conn->client, host, 0);   // host also sets SNI
   br_sslio_init(&conn->io, &conn->client.eng, readChannel, &conn->socketHandle, writeChannel, &conn->socketHandle);
   logInfo("[tls-diag] TLS context initialized host=%s conn=%p handle=%d\n", host, conn, socketHandle);
   return conn;
}

int getLastTlsError(TlsConn *conn) { return br_ssl_engine_last_error(&conn->client.eng); }

int splitHttpsUrl(const char *url, char *hostOut, int hostCap, const char **pathOut)
{
   if (strncmp(url, "https://", 8) != 0) { logError("[tls] not an https url: %s\n", url); return -1; }
   const char *hostStart = url + 8;
   const char *pathStart = strchr(hostStart, '/');
   int hostLen = pathStart ? (int)(pathStart - hostStart) : (int)strlen(hostStart);
   if (hostLen <= 0 || hostLen >= hostCap) return -1;
   memcpy(hostOut, hostStart, hostLen);
   hostOut[hostLen] = '\0';
   *pathOut = pathStart ? pathStart : "/";
   return 0;
}

void closeTlsConn(TlsConn *conn)
{
   if (!conn) return;
   if (!unregisterTlsConnection(conn)) {
      logError("[tls-audit] duplicate-or-untracked conn close conn=%p\n", conn);
      return;
   }
   int handle = conn->socketHandle;
   int tlsError = getLastTlsError(conn);
   int activeConnections;
   unsigned long created, closed, invalid;
   lock(&socketLock);
   activeConnections = activeConnectionCount;
   created = connectionCreatedCount;
   closed = connectionClosedCount;
   invalid = connectionCloseInvalidCount;
   unlock(&socketLock);
   logInfo("[tls-audit] conn-destroy host=%s conn=%p handle=%d bearssl=%d remaining=%lld chunked=%d chunk-left=%lld leftover=%d keepAlive=%d active=%d created=%lu closed=%lu invalid-close=%lu\n",
           conn->host, conn, handle, tlsError, (long long)conn->remaining,
           conn->chunked, (long long)conn->chunkLeft, conn->leftoverLen,
           conn->keepAlive, activeConnections, created, closed, invalid);
   getTlsChannel()->close(handle);
   free(conn);
}

// section: request

static int writeStr(TlsConn *conn, const char *text) { return br_sslio_write_all(&conn->io, text, strlen(text)); }

int sendTlsRequest(TlsConn *conn, const char *method, const char *httpVersion, int keepAlive, const char *host,
                   const char *path, const char *extraHeaders, const void *body, int bodyLen)
{
   conn->keepAlive = keepAlive;
   // written piecewise, not into a fixed buffer: googlevideo videoplayback urls run to ~2 KB and must not
   // be capped. BearSSL coalesces these small writes into TLS records; the first write drives the handshake.
   if (writeStr(conn, method) || writeStr(conn, " ") || writeStr(conn, path) || writeStr(conn, " ") ||
       writeStr(conn, httpVersion) || writeStr(conn, "\r\nHost: ") || writeStr(conn, host) || writeStr(conn, "\r\n")) {
      logError("[tls-diag] request write failed host=%s conn=%p bearssl=%d\n", host, conn, getLastTlsError(conn));
      return -1;
   }
   if (extraHeaders && *extraHeaders && writeStr(conn, extraHeaders)) {
      logError("[tls-diag] extra-header write failed host=%s conn=%p bearssl=%d\n", host, conn, getLastTlsError(conn));
      return -1;
   }
   // a body needs its length, and so does a bodyless write method: PUT/POST/PATCH without a
   // Content-Length is ambiguous to the server (Drive's final, empty upload chunk is one).
   int hasBody = body && bodyLen > 0;
   if (hasBody || (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0)) {
      char contentLength[40];
      snprintf(contentLength, sizeof contentLength, "Content-Length: %d\r\n", hasBody ? bodyLen : 0);
      if (writeStr(conn, contentLength)) {
         logError("[tls-diag] content-length write failed host=%s bearssl=%d\n", host, getLastTlsError(conn));
         return -1;
      }
   }
   if (writeStr(conn, keepAlive ? "Connection: keep-alive\r\n\r\n" : "Connection: close\r\n\r\n")) {
      logError("[tls-diag] request terminator write failed host=%s bearssl=%d\n", host, getLastTlsError(conn));
      return -1;
   }
   if (hasBody && br_sslio_write_all(&conn->io, body, bodyLen) != 0) {
      logError("[tls-diag] request body write failed host=%s bytes=%d bearssl=%d\n", host, bodyLen, getLastTlsError(conn));
      return -1;
   }
   int flushRc = br_sslio_flush(&conn->io);
   if (flushRc != 0) logError("[tls-diag] request flush failed host=%s bearssl=%d\n", host, getLastTlsError(conn));
   return flushRc != 0 ? -1 : 0;
}

// case-insensitive substring test (for the response Connection header).
static int containsCi(const char *haystack, const char *lowerNeedle)
{
   for (; *haystack; haystack++) {
      int i = 0;
      while (lowerNeedle[i]) {
         char c = haystack[i];
         if (c >= 'A' && c <= 'Z') c += 32;
         if (c != lowerNeedle[i]) break;
         i++;
      }
      if (!lowerNeedle[i]) return 1;
   }
   return 0;
}

const char *getTlsHost(TlsConn *conn) { return conn->host; }

void setTlsPoolAllowed(TlsConn *conn, int allowed)
{
   if (conn) conn->poolAllowed = allowed != 0;
}

int getTlsStatusLine(TlsConn *conn, char *out, int cap)
{
   if (!conn || !out || cap <= 0 || !conn->statusLine[0]) return -1;
   int i = 0;
   while (conn->statusLine[i] && i < cap - 1) { out[i] = conn->statusLine[i]; i++; }
   out[i] = '\0';
   return 0;
}

// can this connection be handed back to a pool and reused for the next request to the same host? only if
// we asked to keep it, the whole body was consumed (so the socket sits exactly at the next response), and
// the server agreed (HTTP/1.0 keep-alive is opt-in: the server must echo Connection: keep-alive).
int isTlsReusable(TlsConn *conn)
{
   if (!conn->poolAllowed || !conn->keepAlive || conn->remaining != 0) return 0;
   char connection[48];
   if (getTlsHeader(conn, "connection", connection, sizeof connection) != 0) return 0;
   return containsCi(connection, "keep-alive");
}

// section: response parsing

// index just past the "\r\n\r\n" header/body break in buf[0..len), or -1 if not present yet.
static int findBodyStart(const unsigned char *buf, int len)
{
   for (int i = 0; i + 3 < len; i++)
      if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') return i + 4;
   return -1;
}

// value of a header line (case-insensitive name match), or NULL. `headers` is NUL-terminated header text.
static const char *findHeader(const char *headers, const char *name)
{
   int nameLen = (int)strlen(name);
   for (const char *line = headers; line && *line; ) {
      int match = 1;
      for (int i = 0; i < nameLen; i++) {
         char a = line[i], b = name[i];
         if (a >= 'A' && a <= 'Z') a += 32;
         if (a != b) { match = 0; break; }
      }
      if (match && line[nameLen] == ':') {
         const char *value = line + nameLen + 1;
         while (*value == ' ') value++;
         return value;
      }
      const char *newline = strchr(line, '\n');
      line = newline ? newline + 1 : NULL;
   }
   return NULL;
}

int readTlsHead(TlsConn *conn, int *status, uint64_t *totalSize)
{
   if (status) *status = 0;
   if (totalSize) *totalSize = 0;

   // read until the blank line that ends the head (may pull the first body bytes in with it)
   int headLen = 0, bodyStart = -1;
   uint64_t headStartUs = sys_time_get_system_time();
   while (headLen < HEAD_MAX &&
          sys_time_get_system_time() - headStartUs < (uint64_t)TLS_HEADER_TIMEOUT_MS * 1000ull) {
      int got = br_sslio_read(&conn->io, conn->head + headLen, HEAD_MAX - headLen);
      if (got <= 0) break;
      headLen += got;
      if ((bodyStart = findBodyStart(conn->head, headLen)) >= 0) break;
   }
   if (bodyStart < 0) {
      logError("[tls-diag] response head failed host=%s conn=%p bytes=%d elapsedMs=%llu bearssl=%d\n",
               conn->host, conn, headLen,
               (unsigned long long)((sys_time_get_system_time()-headStartUs)/1000ull), getLastTlsError(conn));
      return -1;
   }

   conn->head[bodyStart - 2] = '\0';   // terminate the header text at the blank line's first \r\n
   const char *headers = (const char *)conn->head;
   const char *lineEnd = strstr(headers, "\r\n");
   int statusLen = lineEnd ? (int)(lineEnd - headers) : 0;
   if (statusLen > (int)sizeof conn->statusLine - 1) statusLen = (int)sizeof conn->statusLine - 1;
   if (statusLen > 0) {
      memcpy(conn->statusLine, headers, (size_t)statusLen);
      conn->statusLine[statusLen] = '\0';
   } else {
      conn->statusLine[0] = '\0';
   }

   if (status) {
      const char *space = strchr(headers, ' ');
      if (strncmp(headers, "HTTP/1.", 7) == 0 && space) *status = atoi(space + 1);
   }

   // body length: Content-Length delimits this response; Content-Range's "/total" gives the full resource.
   // a server that starts sending before it knows the length (youtube's media replies) sends the body in
   // chunks instead, each behind its own size line, which recvTls strips back out.
   const char *length = findHeader(headers, "content-length");
   const char *encoding = findHeader(headers, "transfer-encoding");
   conn->chunked = encoding && strncmp(encoding, "chunked", 7) == 0;
   conn->chunkLeft = 0;
   conn->chunkedEnd = 0;
   conn->remaining = length ? (int64_t)strtoull(length, NULL, 10) : -1;
   if (status && (*status == 204 || *status == 304)) conn->remaining = 0;
   if (totalSize) {
      const char *range = findHeader(headers, "content-range");
      const char *slash = range ? strchr(range, '/') : NULL;
      if (slash) *totalSize = strtoull(slash + 1, NULL, 10);
      else if (conn->remaining > 0) *totalSize = (uint64_t)conn->remaining;
   }

   conn->leftover = conn->head + bodyStart;
   conn->leftoverLen = headLen - bodyStart;
   return 0;
}

int getTlsHeader(TlsConn *conn, const char *name, char *out, int cap)
{
   const char *value = findHeader((const char *)conn->head, name);
   if (!value) return -1;
   int i = 0;
   while (value[i] && value[i] != '\r' && value[i] != '\n' && i < cap - 1) { out[i] = value[i]; i++; }
   out[i] = '\0';
   return 0;
}

// pulls up to cap bytes of the raw stream, the bytes read while parsing the head first. >0 bytes,
// 0 the connection ended cleanly, -1 it failed.
static int64_t readRaw(TlsConn *conn, void *buffer, uint64_t cap)
{
   if (conn->leftoverLen > 0) {
      uint64_t n = (uint64_t)conn->leftoverLen < cap ? (uint64_t)conn->leftoverLen : cap;
      memcpy(buffer, conn->leftover, (size_t)n);
      conn->leftover += n;
      conn->leftoverLen -= (int)n;
      return (int64_t)n;
   }
   int got = br_sslio_read(&conn->io, buffer, (size_t)cap);
   if (got > 0) return got;
   return getLastTlsError(conn) == BR_ERR_OK ? 0 : -1;
}

static int hexValue(unsigned char c)
{
   if (c >= '0' && c <= '9') return c - '0';
   if (c >= 'a' && c <= 'f') return c - 'a' + 10;
   if (c >= 'A' && c <= 'F') return c - 'A' + 10;
   return -1;
}

// each chunk is introduced by its size in hex on a line of its own, and a zero size ends the body. the
// line may carry a ";extension" tail, and the blank line closing the previous chunk's data is skipped
// here rather than tracked separately. 0 when a size was read, -1 on a broken or malformed stream.
static int readChunkSize(TlsConn *conn)
{
   int64_t size = 0;
   int sawDigit = 0, inExtension = 0;
   int lineBytes = 0;
   uint64_t lineStartUs = sys_time_get_system_time();
   for (;;) {
      if (++lineBytes > TLS_CHUNK_LINE_MAX ||
          sys_time_get_system_time() - lineStartUs >= (uint64_t)TLS_HEADER_TIMEOUT_MS * 1000ull)
         return -1;
      unsigned char c;
      int64_t got = readRaw(conn, &c, 1);
      if (got <= 0) return -1;
      if (c == '\n') {
         if (!sawDigit) { inExtension = 0; continue; }   // the empty line after the previous chunk's data
         break;
      }
      if (c == '\r' || inExtension) continue;
      int value = hexValue(c);
      if (value < 0) { inExtension = 1; continue; }
      size = size * 16 + value;
      sawDigit = 1;
   }
   conn->chunkLeft = size;
   conn->chunkedEnd = (size == 0);
   return 0;
}

// serves body bytes out of a chunked stream, so callers never see the framing.
static int64_t recvChunked(TlsConn *conn, void *buffer, uint64_t cap)
{
   if (conn->chunkedEnd) return 0;
   if (conn->chunkLeft == 0 && readChunkSize(conn) != 0) return -1;
   if (conn->chunkedEnd) return 0;

   uint64_t want = cap < (uint64_t)conn->chunkLeft ? cap : (uint64_t)conn->chunkLeft;
   int64_t got = readRaw(conn, buffer, want);
   if (got <= 0) return -1;   // a chunk that stops short is a broken body, not the end of one
   conn->chunkLeft -= got;
   return got;
}

int64_t recvTls(TlsConn *conn, void *buffer, uint64_t cap)
{
   if (cap == 0) return 0;
   if (conn->chunked) return recvChunked(conn, buffer, cap);

   // serve the bytes already read past the head first
   if (conn->leftoverLen > 0) {
      uint64_t n = (uint64_t)conn->leftoverLen < cap ? (uint64_t)conn->leftoverLen : cap;
      memcpy(buffer, conn->leftover, (size_t)n);
      conn->leftover += n;
      conn->leftoverLen -= (int)n;
      if (conn->remaining > 0) conn->remaining -= n;
      return (int64_t)n;
   }

   if (conn->remaining == 0) return 0;   // whole Content-Length delivered

   uint64_t want = cap;
   if (conn->remaining > 0 && (uint64_t)conn->remaining < want) want = (uint64_t)conn->remaining;
   int got = br_sslio_read(&conn->io, buffer, (size_t)want);
   if (got > 0) {
      if (conn->remaining > 0) conn->remaining -= got;
      return got;
   }
   // br_sslio_read <= 0: the connection ended (or errored). For a close-delimited body (no Content-Length,
   // remaining < 0) the close IS the end of the body -> EOF, even on a bare TCP FIN with no TLS close_notify
   // (common for HTTP/1.0 responses; BearSSL flags the missing close_notify as an error, but the HTTP framing
   // is the close). For a length-delimited body a graceful close is EOF, anything else is a real transport error.
   if (conn->remaining < 0) return 0;
   int tlsError = getLastTlsError(conn);
   if (tlsError != BR_ERR_OK)
      logWarn("[tls-audit] body-read-error host=%s conn=%p handle=%d remaining=%lld want=%llu bearssl=%d\n",
              conn->host, conn, conn->socketHandle, (long long)conn->remaining,
              (unsigned long long)want, tlsError);
   return tlsError == BR_ERR_OK ? 0 : -1;
}
