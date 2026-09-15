#ifndef ethernetclient_h
#define ethernetclient_h
#include "Energia.h"
#include "Print.h"
#include "Client.h"
#include "IPAddress.h"
#include "EthernetServer.h"
#include <lwip/dns.h>

/* Set connection timeout to 10 sec */
#define CONNECTION_TIMEOUT 1000 * 10

class EthernetClient : public Client {
public:
	EthernetClient();
	EthernetClient(struct client *c);
	/* `cs` points either at a slot owned by an EthernetServer or at this object's
	 * own client_state. The implicit copy operations copy the pointer verbatim,
	 * so copying a self-owned client left the copy dangling as soon as the source
	 * went out of scope. */
	EthernetClient(const EthernetClient &other);
	EthernetClient &operator=(const EthernetClient &other);

	uint8_t status();
	virtual int connect(IPAddress ip, uint16_t port);
	virtual int connect(const char *host, uint16_t port);
	virtual int connect(IPAddress ip, uint16_t port, unsigned long timeout);
	virtual int connect(const char *host, uint16_t port, unsigned long timeout);
	virtual size_t write(uint8_t);
	virtual size_t write(const uint8_t *buf, size_t size);
	virtual int available();
	/** Bytes that can be queued right now without blocking. write() spins on
	 *  delay(1) when lwIP's send buffer is full, which is unbounded blocking
	 *  inside a PLC scan cycle, so a caller that must not block asks first. */
	int availableForWrite();
	virtual int read();
	virtual int port();
	virtual int read(uint8_t *buf, size_t size);
	virtual int peek();
	virtual void flush();
	virtual void stop();
	virtual uint8_t connected();
	virtual operator bool();
	static err_t do_connected(void *arg, struct tcp_pcb *pcb, err_t err);
	static err_t do_recv(void *arg, struct tcp_pcb *cpcb, struct pbuf *p, err_t err);
	static err_t do_poll(void *arg, struct tcp_pcb *cpcb);
	static void do_err(void * arg, err_t err);
	static void do_dns(const char *name, struct ip_addr *ipaddr, void *arg);
	friend class EthernetServer;
	using Print::write;

private:
	struct client client_state;
	volatile bool _connected;
	struct client *cs;
	/* The generation this handle was created with. See struct client. */
	uint16_t _generation;

	/* True once the slot this handle refers to has been recycled for a different
	 * connection. A stale handle must behave exactly like a closed one: report
	 * not-connected, read nothing, write nothing, and above all not stop(). */
	bool stale() const {
		return (cs != &client_state) && (cs->generation != _generation);
	}

	void copyFrom(const EthernetClient &other);
	int readLocked();
};
#endif
