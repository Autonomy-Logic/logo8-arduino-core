#include "Ethernet.h"
#include "EthernetClient.h"
#include "EthernetServer.h"
#include <string.h>

/* SYNC_FETCH_AND_NULL: atomic{ tmp=*x; *x=NULL; return tmp; } */
#define SYNC_FETCH_AND_NULL(x)   (__sync_fetch_and_and(x, NULL))

EthernetServer::EthernetServer(uint16_t port) {
	_port = port;
	lastConnect = 0;
	_generation = 0;
	lastClient = 0;
	memset(clients, 0, sizeof(clients));
}

err_t EthernetServer::do_poll(void *arg, struct tcp_pcb *cpcb) {
	/* We only end up here if the connection failed to close
	 * in an earlier call to tcp_close */
	err_t err = tcp_close(cpcb);

	if (err != ERR_OK) {
		/* error closing, try again later in polli (every 2 sec) */
		tcp_poll(cpcb, do_poll, 4);
	}

	return err;
}

void EthernetServer::do_close(void *arg, struct tcp_pcb *cpcb) {
	/*
	 * arg is the SLOT this connection was accepted into (set by do_accept),
	 * so there is no lookup to get wrong.
	 *
	 * This used to search the table for `clients[i].port == cpcb->remote_port`.
	 * Remote ports are NOT unique -- two peers on different hosts routinely
	 * pick the same source port, and do_accept happily gives them two slots
	 * carrying the same `port` value. The search then matched whichever came
	 * first, so one connection's close (and, in do_recv, one connection's
	 * DATA) was applied to the other. The pcb pointer is unique for the life
	 * of the connection, and we already store it.
	 */
	struct client * cs = static_cast<struct client*>(arg);

	tcp_arg(cpcb, NULL);
	tcp_recv(cpcb, NULL);
	tcp_err(cpcb, NULL);
	tcp_poll(cpcb, NULL, 0);
	tcp_sent(cpcb, NULL);

	if (cs == NULL || cs->cpcb != cpcb) {
		/* Slot already recycled for a newer connection, or already closed.
		 * Touching it now would corrupt whoever owns it. */
		return;
	}

	/* --- close the connection --- */

	cs->read = 0;
	cs->port = 0;

	if (cs->p) {
		if (cs->cpcb)
			tcp_recved(cpcb, cs->p->tot_len);
		pbuf_free((pbuf*)cs->p);
		cs->p = NULL;
	}
	if (cs->cpcb) {
		err_t err = tcp_close(cpcb);
		if (err != ERR_OK) {
			/* Error closing, try again later in polli (every 2 sec) */
			tcp_poll(cpcb, do_poll, 4);
		}
		cs->cpcb = NULL;
	}

	return;
}

err_t EthernetServer::did_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
	return ERR_OK;
}

err_t EthernetServer::do_recv(void *arg, struct tcp_pcb *cpcb, struct pbuf *p,
		err_t err) {

	/* arg is the slot this connection was accepted into. See do_close() for
	 * why the old remote_port search was wrong. */
	struct client * cs = static_cast<struct client*>(arg);

	/* p==0 for end-of-connection (TCP_FIN packet) */
	if (p == 0) {
		do_close(arg, cpcb);
		return ERR_OK;
	}

	if (cs == NULL || cs->cpcb != cpcb) {
		/* Data for a connection this slot no longer represents. */
		return ERR_MEM;
	}

	if (cs->p != 0)
		pbuf_cat((pbuf*)cs->p, p);
	else
		cs->p = p;

	return ERR_OK;
}

/*
 * Fatal error on an accepted connection -- in practice a RST from the peer.
 *
 * do_accept() used to install no error callback at all, which is a
 * use-after-free: on RST lwIP calls the (NULL) errf and then does
 *
 *     tcp_pcb_remove(&tcp_active_pcbs, pcb); memp_free(MEMP_TCP_PCB, pcb);
 *
 * (tcp_in.c), returning the pcb to the pool while this slot still held a
 * pointer to it and a non-zero port. The next available() then read
 * `cpcb->state` out of a recycled pool entry -- which, being a pool, is very
 * likely a LIVE DIFFERENT connection -- and could hand the sketch a client
 * whose writes go to the wrong peer.
 *
 * lwIP has already freed the pcb by the time we get here, so this must touch
 * nothing but the slot: no tcp_close, no tcp_recved, no tcp_abort.
 */
void EthernetServer::do_err(void *arg, err_t err) {
	(void)err;
	struct client * cs = static_cast<struct client*>(arg);
	if (cs == NULL)
		return;

	cs->cpcb = NULL;   /* the pcb is GONE -- drop the dangling pointer first */
	cs->port = 0;
	cs->read = 0;
	cs->connected = false;
	if (cs->p) {
		pbuf_free((pbuf*)cs->p);
		cs->p = NULL;
	}
}

err_t EthernetServer::do_accept(void *arg, struct tcp_pcb *cpcb, err_t err) {
	/*
	 * Get the server object from the argument
	 * to get access to variables and functions
	 */

	EthernetServer *server = static_cast<EthernetServer*>(arg);

	/* Find free client */
	uint8_t i;
	for (i = 0; i < MAX_CLIENTS; i++) {
		if (server->clients[i].port == 0)
			break;
	}
	if (i >= MAX_CLIENTS) {
		return ERR_MEM;
	}

	struct client * cs = &server->clients[i];

	memset(cs, 0, sizeof(struct client));

	/* Stamp the identity BEFORE the slot goes live. Skip 0, which means
	 * "self-owned, never recycled" to EthernetClient. */
	if (++server->_generation == 0)
		server->_generation = 1;
	cs->generation = server->_generation;
	cs->cpcb = cpcb;
	/* port last: a non-zero port is what marks the slot live to available(),
	 * so everything else must already be consistent when it is set. */
	cs->port = cpcb->remote_port;

	tcp_accepted(server->spcb);

	/* The SLOT, not the server: it is what every per-connection callback
	 * needs, and it is the only thing tcp_err() gets (lwIP passes the
	 * callback arg and no pcb, the pcb being freed by then). */
	tcp_arg(cpcb, cs);
	tcp_recv(cpcb, do_recv);
	tcp_sent(cpcb, did_sent);
	tcp_err(cpcb, do_err);

	/*
	 * Returning ERR_OK indicates to the stack the the
	 * connection has been accepted
	 */
	return ERR_OK;
}

void EthernetServer::begin() {
	spcb = tcp_new();
	tcp_bind(spcb, IP_ADDR_ANY, _port);
	spcb = tcp_listen(spcb);
	tcp_arg(spcb, this);
	tcp_accept(spcb, do_accept);
}

EthernetClient EthernetServer::available() {
	uint8_t i;
	/* Find active client */
	for (i = 0; i < MAX_CLIENTS; i++) {
		if (++lastClient >= MAX_CLIENTS)
			lastClient = 0;
		if (clients[lastClient].port != 0) {
			/* cpcb may change to NULL during interrupt servicing, so avoid the NULL pointer access */
			struct tcp_pcb * cpcb = (tcp_pcb*)clients[lastClient].cpcb;
			if (cpcb && cpcb->state == ESTABLISHED)
				return EthernetClient(&clients[lastClient]);
		}
	}
	/* No client connection active */
	return EthernetClient(NULL);
}

EthernetClient EthernetServer::accept() {
	for (uint8_t i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].port == 0 || clients[i].claimed)
			continue;
		/* cpcb may change to NULL during interrupt servicing */
		struct tcp_pcb * cpcb = (tcp_pcb*)clients[i].cpcb;
		if (cpcb && cpcb->state == ESTABLISHED) {
			clients[i].claimed = true;
			return EthernetClient(&clients[i]);
		}
	}
	/* Nothing new */
	return EthernetClient(NULL);
}

size_t EthernetServer::write(uint8_t b) {
	return write(&b, 1);
}

size_t EthernetServer::write(const uint8_t *buffer, size_t size) {
	uint8_t i;
	size_t n = 0;
	EthernetClient client;

	/* Find connected clients */
	for (i = 0; i < MAX_CLIENTS; i++) {
		if (clients[i].port != 0 && clients[i].cpcb
				&& clients[i].cpcb->state == ESTABLISHED) {
			/* cpcb may change to NULL during interrupt servicing, so avoid the NULL pointer access */
			struct tcp_pcb * cpcb = (tcp_pcb*)clients[i].cpcb;
			if (cpcb && cpcb->state == ESTABLISHED) {
				client = EthernetClient(&clients[i]);
				n += client.write(buffer, size);
			}
		}
	}

	return n;
}
