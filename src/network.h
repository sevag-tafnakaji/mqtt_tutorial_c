#ifndef NETWORK_H
#define NETWORK_H

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

// Socket families
#define UNIX 0
#define INET 1

/**
 * Set TCP_NODELAY flag to true, disable Nagle's alogrithm,
 * no more waiting for incoming packets on the buffer.
 */
int set_tcp_nodelay(int);

/* Auxiliary function for creating epoll server */
int create_and_bind(const char *, const char *, int);

/**
 * Create a non-blocking socket and make it listen to a specified address
 * and port
 */
int make_listen(const char *, const char *, int);

/* Accept a connection and add it to the right epollfd */
int accept_connection(int);

/* I/O management functions */

/**
 * Send all data in a loop, avoiding interruption based on the kernel
 * buffer availability
 */
ssize_t send_bytes(int, const unsigned char *, size_t);

/**
 * Receive (send) an arbitrary number of bytes from a file descriptor
 * and store them in a buffer
 */
ssize_t recv_bytes(int, unsigned char *)

#endif // NETWORK_H
