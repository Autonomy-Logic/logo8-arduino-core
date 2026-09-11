#ifndef ethernetserver_h
#define ethernetserver_h

#include "Server.h"
#include "lwip/tcp.h"

#define MAX_CLIENTS 8

/* 
 * client state structure that is passed on to the client
 * through available()
 */
struct client {
	/* Connection port. (may change to 0 at any time during interrupt servicing) */
	volatile uint16_t port;
	/* Received data buffer. (may change to NULL at any time during interrupt servicing) */
	volatile struct pbuf *p;
	/* tcp control block. (may change to NULL at any time during interrupt servicing) */
	volatile struct tcp_pcb *cpcb;
	volatile bool connected;
	uint16_t read;
	/* true for an OUTBOUND client (one that owns its own client_state), false
	 * for a connection an EthernetServer accepted.
	 *
	 * It no longer gates anything. It used to decide whether write() called
	 * tcp_output(), which meant a server's reply was queued and never pushed
	 * until the 250 ms TCP timer -- see the note in EthernetClient::write().
	 * Kept because it still records which kind of client this is; do NOT
	 * reintroduce it as an output guard. */
	bool mode;
	/*
	 * Connection identity.
	 *
	 * An EthernetClient is a HANDLE onto one of these slots, not an owner of
	 * it. When a connection ends the slot is recycled by do_accept() for the
	 * next arrival, and every EthernetClient the sketch still holds then
	 * silently refers to a DIFFERENT connection: connected() reads true
	 * again, available() returns the new peer's bytes, and a write goes to
	 * the wrong socket. Neither connected() nor available() can detect it.
	 *
	 * `generation` is bumped on every accept, so a handle that captured the
	 * old value can tell. 0 is reserved for a client that owns its own
	 * client_state (an outbound EthernetClient), which is never recycled and
	 * so needs no check.
	 *
	 * The ESP32 core solves the same problem by refcounting the socket with a
	 * shared_ptr, which cannot be done here: lwIP's raw callback API gives us
	 * a fixed slot table rather than a fresh descriptor per connection, and
	 * pinning a slot until the sketch drops its handle would let one leaked
	 * handle retire one of only MAX_CLIENTS slots for good. Detecting the
	 * reuse costs two bytes and cannot leak.
	 */
	volatile uint16_t generation;
	/* Handed out by accept() and not yet re-accepted. Cleared when the slot is
	 * recycled. See EthernetServer::accept(). */
	volatile bool claimed;
};

class EthernetClient;

class EthernetServer : public Server {
private:
	unsigned long lastConnect;
	uint16_t _port;
	struct tcp_pcb *spcb;
	struct client clients[MAX_CLIENTS];
	/* Bumped on every accept to stamp clients[].generation. Never 0. */
	uint16_t _generation;
	/* Round-robin cursor. Was a function-local `static`, which made it SHARED
	 * BY EVERY EthernetServer IN THE IMAGE -- a sketch running Modbus on 502
	 * and a log on 23 had each server advancing the other's cursor over its
	 * own unrelated slot table. Per-instance is the only thing that makes the
	 * round-robin mean anything. */
	uint8_t  lastClient;
	static err_t do_poll(void *arg, struct tcp_pcb *cpcb);
	static void  do_close(void *arg, struct tcp_pcb *cpcb);
	static void  do_err(void *arg, err_t err);
public:
	EthernetServer(uint16_t);
	/* Returns any ESTABLISHED client, round-robin, whether or not it is new
	 * and whether or not it has data. Kept for backward compatibility --
	 * sketches that re-fetch the handle every pass through loop() are fine.
	 * A sketch that KEEPS the returned client across connections wants
	 * accept() instead; see the note on struct client::generation. */
	EthernetClient available();
	/* Hand over each connection exactly ONCE, as Arduino's Ethernet >= 2.0
	 * and the ESP32 core do (which deprecated available() in favour of this).
	 * Returns a false client when there is nothing new. This is what a server
	 * holding per-connection state needs: available() cannot distinguish a
	 * new arrival from a peer it already knows. */
	EthernetClient accept();
	virtual void begin();
	virtual size_t write(uint8_t);
	virtual size_t write(const uint8_t *buf, size_t size);
	static err_t do_accept(void *arg, struct tcp_pcb *pcb, err_t err);
	static err_t do_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);
	static err_t did_sent(void *arg, struct tcp_pcb *pcb, u16_t len);
	using Print::write;
};

#endif
