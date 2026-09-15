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
	/* true for an OUTBOUND client (one that owns its own client_state), false for
	 * a connection an EthernetServer accepted. It no longer gates anything; it
	 * used to decide whether write() called tcp_output(). Kept because it records
	 * which kind of client this is; do NOT reintroduce it as an output guard. */
	bool mode;
	/*
	 * Connection identity. An EthernetClient is a handle onto one of these slots,
	 * not an owner: when a connection ends the slot is recycled by do_accept()
	 * and every handle the sketch still holds silently refers to a different
	 * connection. `generation` is bumped on every accept so a stale handle can
	 * tell; 0 is reserved for a client that owns its own client_state.
	 * Refcounting instead is not possible on lwIP's fixed slot table.
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
	/* Round-robin cursor. Was a function-local `static`, which made it shared by
	 * every EthernetServer in the image, so two servers advanced each other's
	 * cursor over unrelated slot tables. */
	uint8_t  lastClient;
	static err_t do_poll(void *arg, struct tcp_pcb *cpcb);
	static void  do_close(void *arg, struct tcp_pcb *cpcb);
	static void  do_err(void *arg, err_t err);
public:
	EthernetServer(uint16_t);
	/* Returns any ESTABLISHED client, round-robin, whether or not it is new and
	 * whether or not it has data. Kept for backward compatibility. A sketch that
	 * KEEPS the returned client across connections wants accept() instead. */
	EthernetClient available();
	/* Hand over each connection exactly ONCE, as Arduino's Ethernet >= 2.0 and
	 * the ESP32 core do. Returns a false client when there is nothing new. This
	 * is what a server holding per-connection state needs. */
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
