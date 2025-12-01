#define _DEFAULT_SOURCE

#include "network.h"
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

/* Set non-blocking socket */
int set_nonblocking(int fd) {
  int flags, result;
  flags = fcntl(fd, F_GETFL, 0);
  if (flags == -1)
    goto err;
  result = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  if (result == -1)
    goto err;
  return 0;
err:
  perror("set_nonblocking");
  return -1;
}

/* Disable Nagle's algorithm by setting TCP_NODELAY */
int set_tcp_nodelay(int fd) {
  return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &(int){1}, sizeof(int));
}

static int create_and_bind_unix(const char *sockpath) {
  struct sockaddr_un addr;
  int fd;
  if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    perror("socket error");
    return -1;
  }
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sockpath, sizeof(addr.sun_path) - 1);
  unlink(sockpath);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
    perror("bind error");
    return -1;
  }
  return fd;
}

static int create_and_bind_tcp(const char *host, const char *port) {
  struct addrinfo hints = {.ai_family = AF_UNSPEC,
                           .ai_socktype = SOCK_STREAM,
                           .ai_flags = AI_PASSIVE};
  struct addrinfo *result, *rp;
  int sfd;
  if (getaddrinfo(host, port, &hints, &result) != 0) {
    perror("getaddrinfo error");
    return -1;
  }
  for (rp = result; rp != NULL; rp = rp->ai_next) {
    sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sfd == -1)
      continue;
    /* set SO_REUSEADDR so the socket will be reusable after process kill */
    if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0)
      perror("SO_REUSEADDR");
    if ((bind(sfd, rp->ai_addr, rp->ai_addrlen)) == 0) {
      /* successful bind */
      break;
    }
    close(sfd);
  }
  if (rp == NULL) {
    perror("Could not bind");
    return -1;
  }
  freeaddrinfo(result);
  return sfd;
}

int create_and_bind(const char *host, const char *port, int socket_family) {
  int fd;
  if (socket_family == UNIX)
    fd = create_and_bind_unix(host);
  else
    fd = create_and_bind_tcp(host, port);
  return fd;
}

/**
 * Create a non-blocking socket and make it listen on the specified
 * address and family
 */
int make_listen(const char *host, const char *port, int socket_family) {
  int sfd;
  if ((sfd = create_and_bind(host, port, socket_family)) == -1)
    abort();
  if ((set_nonblocking(sfd)) == -1)
    abort();
  //   Set TCP_NODELAY only for TCP sockets
  if (socket_family == INET)
    set_tcp_nodelay(sfd);
  if ((listen(sfd, conf->tcp_backlog)) == -1) {
    perror("listen");
    abort();
  }
  return sfd;
}

int accept_connection(int serversock) {
  int clientsock;
  struct sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);
  if ((clientsock = accept(serversock, (struct sockaddr *)&addr, &addrlen)) < 0)
    return -1;
  set_nonblocking(clientsock);
  // set TCP_NODELAY only for TCP sockets
  if (conf->socket_family == INET)
    set_tcp_nodelay(clientsock);
  char ip_buff[INET_ADDRSTRLEN + 1];
  if (inet_ntop(AF_INET, &addr.sin_addr, ip_buff, sizeof(ip_buff)) == NULL) {
    close(clientsock);
    return -1;
  }
  return clientsock;
}

ssize_t send_bytes(int fd, const unsigned char *buf, size_t len) {
  size_t total = 0;
  size_t bytesleft = len;
  ssize_t n = 0;
  while (total < len) {
    // n is total number of bytes sent
    n = send(fd, buf + total, bytesleft, MSG_NOSIGNAL);
    if (n == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      else
        goto err;
    }
    total += n;
    bytesleft -= n;
  }
  return total;
err:
  fprintf(stderr, "send(2) - error sending data: %s", strerror(errno));
  return -1;
}

/**
 * Receive a given number of bytes on the descriptor fd, storing the
 * stream of data into a 2 Mb capped buffer
 */
ssize_t recv_bytes(int fd, unsigned char *buf, size_t bufsize) {
  ssize_t n = 0;
  ssize_t total = 0;
  while (total < (ssize_t)bufsize) {
    if ((n = recv(fd, buf, bufsize - total, 0)) < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      else
        goto err;
    }
    if (n == 0)
      return 0;
    buf += n;
    total += n;
  }
  return total;
err:
  fprintf(stderr, "recv(2) - error reading data: %s", strerror(errno));
  return -1;
}
