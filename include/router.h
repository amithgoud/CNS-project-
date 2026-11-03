/*
 * router.h - Maps (method, path) of a parsed request to a page handler.
 */
#ifndef ROUTER_H
#define ROUTER_H

#include "http.h"
#include "server.h"

/* Handle one request and send the response. Returns the HTTP status sent,
 * or -1 if the response could not be delivered. */
int router_handle(const conn_t *c, const http_request_t *req);

#endif /* ROUTER_H */
