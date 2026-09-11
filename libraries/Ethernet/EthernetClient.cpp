#include "Energia.h"

#include "Ethernet.h"
#include "EthernetClient.h"
#include "EthernetServer.h"
#include <string.h>

#include "driverlib/interrupt.h"

/* directives for disabling and enabling interrupts */
#define INT_PROTECT_INIT(x)    int x = 0
#define INT_PROTECT(x)         x=IntMasterDisable()
#define INT_UNPROTECT(x)       do{if(!x)IntMasterEnable();}while(0)

/* SYNC_FETCH_AND_NULL: atomic{ tmp=*x; *x=NULL; return tmp; } */
#define SYNC_FETCH_AND_NULL(x)   (__sync_fetch_and_and(x, NULL))

EthernetClient::EthernetClient() {
	_connected = false;
	cs = &client_state;
	cs->mode = true;
	cs->cpcb = NULL;
	cs->p = NULL;
	cs->generation = 0;   /* self-owned: never recycled, never stale */
	_generation = 0;
}

EthernetClient::EthernetClient(struct client *c) {
	if (c == NULL) {
		_connected = false;
		cs = &client_state;
		cs->cpcb = NULL;
		cs->p = NULL;
		cs->generation = 0;
		_generation = 0;
		return;
	}
	_connected = true;
	cs = c;
	cs->mode = false;
	/* Capture the identity of the connection occupying the slot RIGHT NOW.
	 * If the slot is later recycled, stale() sees the mismatch. */
	_generation = c->generation;
}

void EthernetClient::copyFrom(const EthernetClient &other) {
	/* struct client has volatile members, so copy the bytes rather than
	 * relying on an implicit member-wise copy. */
	memcpy((void*)&client_state, (const void*)&other.client_state,
	       sizeof(struct client));
	_connected  = other._connected;
	_generation = other._generation;
	/* Re-point at OUR OWN storage when the source was self-owned; otherwise
	 * both handles legitimately refer to the same server slot. */
	cs = (other.cs == &other.client_state) ? &client_state : other.cs;
}

EthernetClient::EthernetClient(const EthernetClient &other) {
	copyFrom(other);
}

EthernetClient &EthernetClient::operator=(const EthernetClient &other) {
	if (this != &other)
		copyFrom(other);
	return *this;
}

/* Retry a close that could not complete. Deliberately ignores `arg`: the
 * accepted-connection callback arg is a struct client*, and the old code
 * installed EthernetClient::do_poll here, which casts arg to EthernetClient*
 * -- a type confusion that read a server or a slot as if it were a client
 * object. Nothing about retrying a close needs either. */
static err_t close_retry_poll(void *arg, struct tcp_pcb *cpcb) {
	(void)arg;
	err_t err = tcp_close(cpcb);
	if (err != ERR_OK)
		tcp_poll(cpcb, close_retry_poll, 4);
	return err;
}

err_t EthernetClient::do_poll(void *arg, struct tcp_pcb *cpcb) {
	EthernetClient *client = static_cast<EthernetClient*>(arg);

	if (client->_connected) {
		if (cpcb->keep_cnt_sent++ > 4) {
			cpcb->keep_cnt_sent = 0;
			/* Stop polling */
			tcp_poll(cpcb, NULL, 0);
			tcp_abort(cpcb);
			if (client->cs->cpcb == cpcb) /* cs may be already re-used by another connection */
			{
				client->cs->port = 0;
				client->cs->cpcb = 0;
			}
			return ERR_ABRT; /* calling tcp_abort() must return ERR_ABRT */
		}
		/* Send tcp keep alive probe */
		tcp_keepalive(cpcb);
		return ERR_OK;
	}

	/* We only end up here if the connection failed to close
	 * in an earlier call to tcp_close */
	err_t err = tcp_close(cpcb);

	if (err != ERR_OK) {
		/* error closing, try again later in poll (every 2 sec) */
		tcp_poll(cpcb, do_poll, 4);
	}

	if (client->cs->cpcb == cpcb) /* cs may be already re-used by another connection */
	{
		client->cs->cpcb = 0;
		client->cs->port = 0;
	}

	return err;
}

void EthernetClient::do_err(void * arg, err_t err) {
	EthernetClient *client = static_cast<EthernetClient*>(arg);

	if (client->_connected) {
		client->_connected = false;
		return;
	}

	/*
	 * Set connected to true to finish connecting.
	 * ::connect wil take care of figuring out if we are truly connected
	 * by looking at the socket state
	 */
	client->_connected = true;
}

err_t EthernetClient::do_connected(void * arg, struct tcp_pcb * tpcb,
		err_t err) {
	EthernetClient *client = static_cast<EthernetClient*>(arg);

	client->_connected = true;

	return err;
}

err_t EthernetClient::do_recv(void *arg, struct tcp_pcb *cpcb, struct pbuf *p,
		err_t err) {
	/*
	 * Get the client object from the argument
	 * to get access to variables and functions
	 */
	EthernetClient *client = static_cast<EthernetClient*>(arg);

	/* p==0 for end-of-connection (TCP_FIN packet) */
	if (p == 0) {
		client->_connected = false;
		return ERR_OK;
	}

	if (client->cs->p != 0)
		pbuf_cat((pbuf*)client->cs->p, p);
	else
		client->cs->p = p;

	return ERR_OK;
}

void EthernetClient::do_dns(const char *name, struct ip_addr *ipaddr,
		void *arg) {
	ip_addr_t *result = (ip_addr_t *) arg;

	/* BEWARE: lwip stack has been modified to set ipaddr
	 * to IPADDR_NONE if the lookup failed */
	result->addr = ipaddr->addr;
}

int EthernetClient::connect(const char* host, uint16_t port) {
  return connect(host, port, CONNECTION_TIMEOUT);
}

int EthernetClient::connect(const char* host, uint16_t port, unsigned long timeout) {
	ip_addr_t ip;
	ip.addr = 0;

	dns_gethostbyname(host, &ip, do_dns, &ip);

	while (!ip.addr) {
		delay(10);
	}

	if (ip.addr == IPADDR_NONE)
		return false;

	return (connect(IPAddress(ip.addr), port, timeout));
}

int EthernetClient::connect(IPAddress ip, uint16_t port) {
  return connect(ip, port, CONNECTION_TIMEOUT);
}

int EthernetClient::connect(IPAddress ip, uint16_t port, unsigned long timeout) {
	ip_addr_t dest;
	dest.addr = ip;

	cs->cpcb = tcp_new();
	cs->read = 0;
	cs->p = NULL;

	if (cs->cpcb == NULL) {
		return false;
	}

	tcp_arg((tcp_pcb*)cs->cpcb, this);
	tcp_recv((tcp_pcb*)cs->cpcb, do_recv);
	tcp_err((tcp_pcb*)cs->cpcb, do_err);

	uint8_t val = tcp_connect((tcp_pcb *)cs->cpcb, &dest, port, do_connected);

	if (val != ERR_OK) {
		return false;
	}

	/* Wait for the connection.
	 * Abort if the connection does not succeed within the prescribed timeout */
	unsigned long then = millis();

	while (!_connected) {
		unsigned long now = millis();
		delay(10);
		if (now - then > timeout) {
			tcp_close((tcp_pcb*)cs->cpcb);
			cs->cpcb = NULL;
			return false;
		}
	}

	if (cs->cpcb->state != ESTABLISHED) {
		_connected = false;
		tcp_close((tcp_pcb*)cs->cpcb);
		cs->cpcb = NULL;
		return false;
	}

	/* Poll to determine if the peer is still alive */
	tcp_poll((tcp_pcb*)cs->cpcb, do_poll, 10);
	return _connected;
}

size_t EthernetClient::write(uint8_t b) {
	return write(&b, 1);
}

size_t EthernetClient::write(const uint8_t *buf, size_t size) {
	uint32_t i = 0, inc = 0;
	boolean stuffed_buffer = false;

	if (stale())
		return 0;   /* the slot belongs to someone else now */

	struct tcp_pcb * cpcb = (tcp_pcb*)cs->cpcb; /* cs->cpcb may change to NULL during interrupt servicing */

	if (!cpcb)
		return 0;

	// Attempt to write in 1024-byte increments.
	while (i < size) {
		inc = (size - i) < 1024 ? size - i : 1024;
		err_t err = tcp_write(cpcb, buf + i, inc, TCP_WRITE_FLAG_COPY);
		if (err != ERR_MEM) {
			// Keep enqueueing the lwIP buffer until it's full...
			i += inc;
			stuffed_buffer = false;
		} else {
			if (!stuffed_buffer) {
				// Buffer full; force output
				if (cs->mode)
					tcp_output(cpcb);
				stuffed_buffer = true;
			} else {
				delay(1); // else wait a little bit for lwIP to flush its buffers
			}
		}
	}
	// flush any remaining queue contents
	if (!stuffed_buffer) {
		if (cs->mode)
			tcp_output(cpcb);
	}

	return size;
}

int EthernetClient::available() {
	if (stale())
		return 0;
	struct pbuf * p = (pbuf*)cs->p; /* cs->p may change to NULL during interrupt servicing */
	if (!p)
		return 0;
	return p->tot_len - cs->read;
}

int EthernetClient::port() {
	if (stale())
		return 0;
	return cs->port;
}

int EthernetClient::readLocked() {
	INT_PROTECT_INIT(oldLevel);

	/* protect the code from preemption of the ethernet interrupt servicing */
	INT_PROTECT(oldLevel);

	if (!available()) {
		INT_UNPROTECT(oldLevel);
		return -1;
	}

	uint8_t *buf = (uint8_t *) cs->p->payload;
	uint8_t b = buf[cs->read];
	cs->read++;

	/* Indicate data was received only if still connected */
	if (cs->cpcb) {
		tcp_recved((tcp_pcb*)cs->cpcb, cs->read);
	}

	/* Read any data still in the buffer regardless of connection state */
	if ((cs->read == cs->p->len) && cs->p->next) {
		cs->read = 0;
		struct pbuf * q = (pbuf*)cs->p;
		cs->p = cs->p->next;
		/* Increase ref count on p->next
		 * 1->3->1->etc */
		pbuf_ref((pbuf*)cs->p);
		/* Free p which decreases ref count of the chain
		 * and frees up to p->next in this case
		 * ...->1->1->etc */
		pbuf_free(q);
	} else if (cs->read == cs->p->len) {
		cs->read = 0;
		pbuf_free((pbuf*)cs->p);
		cs->p = NULL;
	}

	INT_UNPROTECT(oldLevel);

	return b;
}

int EthernetClient::read() {
	return readLocked();
}

int EthernetClient::read(uint8_t *buf, size_t size) {
	uint16_t i;
	int b;

	if (available() <= 0)
		return -1;

	/* TODO: Replace lazy method with optimized on */
	for (i = 0; i < size; i++) {
		b = read();
		if (b == -1)
			break;
		buf[i] = b;
	}

	return i;
}

int EthernetClient::peek() {
	INT_PROTECT_INIT(oldLevel);
	uint8_t b;

	/* protect code from preemption of the ethernet interrupt servicing */
	INT_PROTECT(oldLevel);

	if (!available()) {
		INT_UNPROTECT(oldLevel);
		return -1;
	}

	uint8_t *buf = (uint8_t *) cs->p->payload;
	b = buf[cs->read];

	INT_UNPROTECT(oldLevel);

	return b;
}

void EthernetClient::flush() {
	INT_PROTECT_INIT(oldLevel);
	/* protect code from preemption of the ethernet interrupt servicing */
	INT_PROTECT(oldLevel);
	if (available()) {
		cs->read = cs->p->tot_len;
		tcp_recved((tcp_pcb*)cs->cpcb, 0);
	}
	INT_UNPROTECT(oldLevel);
}

void EthernetClient::stop() {
	/* Stop frees any resources including any unread buffers */
	err_t err;

	INT_PROTECT_INIT(oldLevel);

	/* protect the code from preemption of the ethernet interrupt servicing */
	INT_PROTECT(oldLevel);

	if (stale()) {
		/* Closing this would tear down whichever connection now owns the
		 * slot. Just forget it: our own connection is long gone. */
		_connected = false;
		INT_UNPROTECT(oldLevel);
		return;
	}

	struct tcp_pcb * cpcb_copy = (tcp_pcb *) SYNC_FETCH_AND_NULL(&cs->cpcb);
	struct pbuf * p_copy = (pbuf *) SYNC_FETCH_AND_NULL(&cs->p);
	_connected = false;
	cs->port = 0;

	if (cpcb_copy) {
		/* Detach every callback before closing: after this point the slot
		 * this pcb pointed at may be recycled, and a late callback carrying
		 * the old arg must not run. */
		tcp_arg(cpcb_copy, NULL);
		tcp_recv(cpcb_copy, NULL);
		tcp_sent(cpcb_copy, NULL);
		tcp_err(cpcb_copy, NULL);

		if (p_copy) {
			tcp_recved(cpcb_copy, p_copy->tot_len);
			pbuf_free(p_copy);
		}

		err = tcp_close(cpcb_copy);

		if (err != ERR_OK) {
			/* Error closing, try again later in poll (every 2 sec) */
			tcp_poll(cpcb_copy, close_retry_poll, 4);
		}
	}

	INT_UNPROTECT(oldLevel);
}

uint8_t EthernetClient::connected() {
	/* _connected is a flag on THIS handle, so it stays true after the slot has
	 * been recycled -- the stale() test has to come first or a handle onto
	 * someone else's connection reports itself connected. */
	if (stale())
		return 0;

	if (cs != &client_state) {
		/*
		 * Server-accepted connection: liveness is entirely a property of the
		 * SLOT, so do not consult _connected at all.
		 *
		 * _connected is the outbound-connect handshake flag (do_connected /
		 * do_err set it while ::connect spins). The server-side constructor
		 * also sets it true, and nothing ever clears it -- so once a peer
		 * disconnected with no new connection replacing it, connected()
		 * returned TRUE FOREVER: available() is 0 and status() is CLOSED, but
		 * `|| _connected` carried the result. A sketch that retires a
		 * connection on !connected() -- which is the documented idiom -- would
		 * therefore never retire that one, and the slot leaked until some
		 * unrelated peer happened to recycle it.
		 */
		return (available() || (status() == ESTABLISHED));
	}

	/* TODO: test the local client mode and _connected flag */
	return (available() || (status() == ESTABLISHED) || _connected);
}

uint8_t EthernetClient::status() {
	if (stale())
		return CLOSED;
	struct tcp_pcb * cpcb = (tcp_pcb*)cs->cpcb;
	if (cpcb == NULL)
		return CLOSED;
	return cpcb->state;
}

EthernetClient::operator bool() {
	return (status() == ESTABLISHED);
}
