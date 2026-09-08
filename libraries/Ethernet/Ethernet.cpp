#include <Energia.h>
#include <Ethernet.h>
#include <inc/hw_ints.h>
#include <lwip/inet.h>
#include <IPAddress.h>

#define ETHERNET_INT_PRIORITY   0xC0

void EthernetClass::begin(uint8_t *mac_address, IPAddress local_ip, IPAddress dns_server, IPAddress gateway, IPAddress subnet)
{
	uint32_t ui32User0, ui32User1;

	registerSysTickCb(lwIPTimer);
	ROM_FlashUserGet(&ui32User0, &ui32User1);

	/*
	 * Convert the 24/24 split MAC address from NV ram into a 32/16 split
	 * MAC address needed to program the hardware registers, then program
	 * the MAC address into the Ethernet Controller registers.
	 */
	/* MAC selection, in order of preference:
	 *   1. an address already present in EMAC_ADDR0H/L (read before lwIPInit
	 *      resets the EMAC), for parts where USER0/USER1 are unprogrammed;
	 *   2. USER0/USER1 if programmed;
	 *   3. a fixed locally-administered fallback. */
	uint32_t emacH = (*(volatile uint32_t *)0x400EC040u);   /* EMAC_ADDR0H: [15:0]=mac5,mac4 */
	uint32_t emacL = (*(volatile uint32_t *)0x400EC044u);   /* EMAC_ADDR0L: mac3..mac0 */
	if (emacL != 0xFFFFFFFFu && (emacH & 0xFFFFu) != 0xFFFFu && (emacL || (emacH & 0xFFFFu))) {
		pui8MACArray[0] = (emacL & 0xff); pui8MACArray[1] = ((emacL >> 8) & 0xff);
		pui8MACArray[2] = ((emacL >> 16) & 0xff); pui8MACArray[3] = ((emacL >> 24) & 0xff);
		pui8MACArray[4] = (emacH & 0xff); pui8MACArray[5] = ((emacH >> 8) & 0xff);
	} else if (ui32User0 == 0xFFFFFFFFu && ui32User1 == 0xFFFFFFFFu) {
		pui8MACArray[0] = 0x02; pui8MACArray[1] = 0x00; pui8MACArray[2] = 0x00;
		pui8MACArray[3] = 0x00; pui8MACArray[4] = 0x00; pui8MACArray[5] = 0x01;
	} else {
		pui8MACArray[0] = ((ui32User0 >>  0) & 0xff);
		pui8MACArray[1] = ((ui32User0 >>  8) & 0xff);
		pui8MACArray[2] = ((ui32User0 >> 16) & 0xff);
		pui8MACArray[3] = ((ui32User1 >>  0) & 0xff);
		pui8MACArray[4] = ((ui32User1 >>  8) & 0xff);
		pui8MACArray[5] = ((ui32User1 >> 16) & 0xff);
	}

	ROM_IntPrioritySet(INT_EMAC0, ETHERNET_INT_PRIORITY);

	if(!subnet) {
		if((local_ip >> 31) == CLASS_A)
			subnet = CLASS_A_SUBNET;
		else if((local_ip >> 30) == CLASS_B)
			subnet = CLASS_B_SUBNET;
		else if((local_ip >> 29) == CLASS_C)
			subnet = CLASS_C_SUBNET;
	}

	lwIPInit(F_CPU, pui8MACArray, htonl(local_ip), htonl(subnet), htonl(gateway), !local_ip ? IPADDR_USE_DHCP:IPADDR_USE_STATIC);

	lwIPDNSAddrSet((uint32_t)dns_server);
}

void EthernetClass::begin(uint8_t *mac_address, IPAddress local_ip, IPAddress dns_server)
{
	/* Assume the gateway will be the machine on the same network as the local IP
	 * but with last octet being '1' */
	IPAddress gateway = local_ip;
	gateway[3] = 1;
	begin(mac_address, local_ip, dns_server, gateway);
}

void EthernetClass::begin(uint8_t *mac_address, IPAddress local_ip, IPAddress dns_server, IPAddress gateway)
{
	begin(mac_address, local_ip, dns_server, gateway, IPAddress(0,0,0,0));
}

void EthernetClass::begin(uint8_t *mac_address, IPAddress local_ip)
{
	/* Assume the DNS server will be the machine on the same network as the local IP
	 * but with last octet being '1' */
	IPAddress dns_server = local_ip;
	dns_server[3] = 1;
	begin(mac_address, local_ip, dns_server);
}

int EthernetClass::begin(uint8_t *mac_address)
{
	begin(mac_address, IPAddress(0,0,0,0), IPAddress(0,0,0,0), IPAddress(0,0,0,0), IPAddress(0,0,0,0));
	return lwIPDHCPWaitLeaseValid();
}

int EthernetClass::maintain()
{
	/* Always return 0 which for Arduino is DHCP_CHECK_NONE */
	return 0;
}

IPAddress EthernetClass::localIP()
{
	return lwIPLocalIPAddrGet();
}

IPAddress EthernetClass::gatewayIP()
{
	return lwIPLocalGWAddrGet();
}

IPAddress EthernetClass::subnetMask()
{
	return lwIPLocalNetMaskGet();
}

IPAddress EthernetClass::dnsServerIP()
{
	return lwIPDNSAddrGet();
}

uint8_t* EthernetClass::macAddress(uint8_t* mac)
{
	/* If the size of the array is not large enough 
	 * to fit the mac then simply return mac unmodified */

	memcpy(mac, pui8MACArray, 6);
}

void EthernetClass::enableLinkLed()
{
	ROM_GPIOPinConfigure(LINK_LED);
	GPIOPinTypeEthernetLED(LINK_LED_BASE, LINK_LED_PIN);
}

void EthernetClass::enableActivityLed()
{
	ROM_GPIOPinConfigure(ACTIVITY_LED);
	GPIOPinTypeEthernetLED(ACTIVITY_LED_BASE, ACTIVITY_LED_PIN);
}

EthernetClass Ethernet;
