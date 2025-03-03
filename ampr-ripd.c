/*
 * ampr-ripd.c - AMPR 44net RIPv2 Listner Version 1.15
 *
 * Author: Marius Petrescu, YO2LOJ, <marius@yo2loj.ro>
 *
 *
 *
 * Compile with: gcc -O2 -o ampr-ripd ampr-ripd.c
 *
 *
 * Usage: ampr-ripd [-?|-h] [-d] [-v] [-s] [-r] [-i <interface>] [-t <table>] [-a <ip|hostname|subnet>[,<ip|hostname|subnet>...]] [-p <password>] [-m <metric>] [-w <window>] [-f <interface>] [-e <ip>] [-x <system command>]
 *
 * Options:
 *          -?, -h                Usage info
 *          -d                    Debug mode: no daemonization, verbose output
 *          -v                    More verbose debug output
 *                                Using this option without debug leaves the console attached
 *          -s                    Save routes to /var/lib/ampr-ripd/encap.txt (encap format),
 *                                If this file exists, it will be loaded on startup regardless
 *                                of this option
 *          -r                    Compatibility only (ignored, raw sockets are always used)
 *          -i <interface>        Tunnel interface to use, defaults to 'tunl0'
 *          -t <table>            Routing table to use, defaults to 'main'
 *          -a  <ip>[,<ip>...]    Comma separated list of IPs/hostnames or encap style entries to be ignored
 *                                (max. 10 hostnames or IPs, unlimited encap entries)
 *                                The list contains local interface IPs by default
 *          -p <password>         RIPv2 password, defaults to the current valid password
 *          -m <metric>           Use given route metric to set routes, defaults to 0
 *          -w <window>           Sets TCP window size to the given value
 *                                A value of 0 skips window setting. Defaults to 840
 *          -f <interface>        Interface for RIP forwarding, defaults to none/disabled
 *          -e <ip>               Forward destination IP, defaults to 224.0.0.9 if enabled
 *          -x <system command>   Execute this system command after route set/change
 *
 *
 * Observation: All routes are created with protocol set to 44
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *
 * Version History
 * ---------------
 *    0.9    14.Apr.2013    Alpha release, based on Hessus's rip44d
 *    1.0     1.Aug.2013    First functional version, no tables, no tcp window setting
 *    1.1     1.Aug.2013    Fully functional version
 *    1.2     3.Aug.2013    Added option for using raw sockets instead of multicast
 *    1.3     7.Aug.2013    Minor bug fix, removed compiler warnings
 *    1.4     8.Aug.2013    Possible buffer overflow fixed
 *                          Reject metric 15 packets fixed
 *    1.5    10.Aug.2013    Corrected a stupid netmask calculation error introduced in v1.4
 *    1.6    10.Oct.2013    Changed multicast setup procedures to be interface specific (Tnx. Rob, PE1CHL)
 *    1.7     8.Feb.2014    Added support for dynamic hostnames and ampr subnets in the ignore list
 *    1.8    11.Feb.2014    Added option for route metric setting
 *    1.9    13.Feb.2014    Added window size setting option and console detaching on daemon startup
 *    1.10   14.Feb.2014    Small fixes on option and signal processing (Tnx. Demetre, SV1UY)
 *                          Use daemon() instead of fork()
 *                          Option -v without debug keeps the console attached
 *    1.11   17.Feb.2014    Changed netlink route handling to overwrite/delete only routes written by ampr-ripd
 *    1.12   16.Nov.2014    Added the execution of a system command after route setting/changing. This is done
 *                          on startup with encap file present and 30 seconds after RIP update if encap changes
 *                          (Tnx. Rob, PE1CHL for the idea)
 *    1.13   20.Nov.2014    Ignore subnets for which the gateway is inside their own subnet
 *                          Reconstruct forwarded RIP messages to be able to send them even on ampr-gw outages
 *                          Forwarded RIP messages do not use authentication anymore
 *                          Forwarded RIP messages are sent 30 seconds after a RIP update, otherwise every 29 seconds
 *    1.14   21.Sep.2016    Password is included in the daemon. Only need to set should it ever change
 *                          (OK from Brian Kantor - Tnx.)
 *                          Added man page courtesy of Ana C. Custura and the DebianHams
 *    1.15   21.Sep.2016    Removed multicast access mode, now only raw sockets are used
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <asm/types.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <net/route.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <time.h>
#include <ctype.h>

#define AMPR_RIPD_VERSION	"1.15"

#define RTSIZE		1000	/* maximum number of route entries */
#define EXPTIME		600	/* route expiration in seconds */

#define RTFILE		"/var/lib/ampr-ripd/encap.txt"	/* encap file */

#define RTAB_FILE	"/etc/iproute2/rt_tables"	/* route tables */

#define	BUFFERSIZE	8192
#define MYIPSIZE	25	/* max number of local interface IPs */
#define MAXIGNORE	10	/* max number of hosts in the ignore list */

#define FALSE	0
#define TRUE	1

#define RIP_HDR_LEN		4
#define RIP_ENTRY_LEN		(2+2+4*4)
#define RIP_CMD_REQUEST		1
#define RIP_CMD_RESPONSE	2
#define RIP_AUTH_PASSWD		2
#define RIP_AF_INET		2


#define RTPROT_AMPR		44

#define PERROR(s)				fprintf(stderr, "%s: %s\n", (s), strerror(errno))
#define rip_pcmd(cmd)				((cmd==1)?("Request"):((cmd==2)?("Response"):("Unknown")))
#define NLMSG_TAIL(nmsg)			((struct rtattr *) (((void *) (nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))
#define addattr32(n, maxlen, type, data) 	addattr_len(n, maxlen, type, &data, 4)
#define rta_addattr32(rta, maxlen, type, data)	rta_addattr_len(rta, maxlen, type, &data, 4)

/* uncomment if types not available */
/*
typedef unsigned char		uint8_t;
typedef unsigned short		uint16_t;
typedef unsigned int		uint32_t;
typedef signed char		sint8_t;
typedef signed short		sint16_t;
typedef signed int		sint32_t;

*/

typedef enum
{
    ROUTE_ADD,
    ROUTE_DEL,
    ROUTE_GET
} rt_actions;

typedef struct __attribute__ ((__packed__))
{
    uint8_t command;
    uint8_t version;
    uint16_t zeros;
} rip_header;


typedef struct __attribute__ ((__packed__))
{
    uint16_t af;
    uint16_t rtag;
    uint32_t address;
    uint32_t mask;
    uint32_t nexthop;
    uint32_t metric;
} rip_entry;

typedef struct __attribute__ ((__packed__))
{
    uint16_t auth;
    uint16_t type;
    uint8_t pass[16];
} rip_auth;

typedef struct
{
    uint32_t address;
    uint32_t netmask;
    uint32_t nexthop;
    time_t timestamp;
} route_entry;


typedef struct
{
    rip_header header;
    rip_entry entries[25];
} rip_packet;


static char *usage_string = "\nAMPR RIPv2 daemon " AMPR_RIPD_VERSION "by Marius, YO2LOJ\n\nUsage: ampr-ripd [-d] [-v] [-s] [-r] [-i <interface>]  [-t <table>] [-a <ip|hostname|subnet>[,<ip|hostname|subnet>...]] [-p <password>] [-m <metric>] [-w <window>] [-f <interface>] [-e <ip>] [-x <system command>]\n";


int debug = FALSE;
int verbose = FALSE;
int save = FALSE;
char *tunif = "tunl0";
unsigned int tunidx = 0;
unsigned int tunaddr;
char *ilist = NULL;
uint32_t ignore_ip[MAXIGNORE];

char *passwd = "pLaInTeXtpAsSwD";
char *table = NULL;
int nrtable;
uint32_t rmetric = 0;
uint32_t rwindow = 840;
char *fwif = NULL;
char *fwdest = "224.0.0.9";
char *syscmd = NULL;

int tunsd;
int fwsd;
int seq;
int updated = FALSE;
int update_encap = FALSE;
int dns_ignore_lookup = FALSE;
int encap_ignore = FALSE;

route_entry routes[RTSIZE];

uint32_t myips[MYIPSIZE];


char * rta_enum (unsigned int rta_type, char* str)
{
char * chtype = NULL;
switch (rta_type)
  {
    case RTA_UNSPEC:
        chtype = "RTA_UNSPEC";
        break;
    case RTA_DST:
        chtype = "RTA_DST";
        break;
    case RTA_SRC:
        chtype = "RTA_SRC";
        break;
    case RTA_IIF:
        chtype = "RTA_IIF";
        break;
    case RTA_OIF:
        chtype = "RTA_OIF";
        break;
    case RTA_GATEWAY:
        chtype = "RTA_GATEWAY";
        break;
    case RTA_PRIORITY:
        chtype = "RTA_PRIORITY";
        break;
    case RTA_PREFSRC:
        chtype = "RTA_PREFSRC";
        break;
    case RTA_METRICS:
        chtype = "RTA_METRICS";
        break;
    case RTA_MULTIPATH:
        chtype = "RTA_MULTIPATH";
        break;
    case RTA_PROTOINFO:
        chtype = "RTA_PROTOINFO";
        break;
    case RTA_FLOW:
        chtype = "RTA_FLOW";
        break;
    case RTA_CACHEINFO:
        chtype = "RTA_CACHEINFO";
        break;
    case RTA_SESSION:
        chtype = "RTA_SESSION";
        break;
    case RTA_MP_ALGO:
        chtype = "RTA_MP_ALGO";
        break;
    case RTA_TABLE:
        chtype = "RTA_TABLE";
        break;
    case RTA_MARK:
        chtype = "RTA_MARK";
        break;
    case RTA_MFC_STATS:
        chtype = "RTA_MFC_STATS";
        break;
    case RTA_VIA:
        chtype = "RTA_VIA";
        break;
    case RTA_NEWDST:
        chtype = "RTA_NEWDST";
        break;
    case RTA_PREF:
        chtype = "RTA_PREF";
        break;
    case RTA_ENCAP_TYPE:
        chtype = "RTA_ENCAP_TYPE";
        break;
    case RTA_ENCAP:
        chtype = "RTA_ENCAP";
        break;
    case RTA_EXPIRES:
        chtype = "RTA_EXPIRES";
        break;
    case RTA_PAD:
        chtype = "RTA_PAD";
        break;
    case RTA_UID:
        chtype = "RTA_UID";
        break;
    case RTA_TTL_PROPAGATE:
        chtype = "RTA_TTL_PROPAGATE";
        break;
    case RTA_IP_PROTO:
        chtype = "RTA_IP_PROTO";
        break;
    case RTA_SPORT:
        chtype = "RTA_SPORT";
        break;
    case RTA_DPORT:
        chtype = "RTA_DPORT";
        break;
    case RTA_NH_ID:
        chtype = "RTA_NH_ID";
        break;
    case __RTA_MAX:
        chtype = "__RTA_MAX";
        break;
    default:
        chtype = "other";
        break;
  }
  if (strlen(chtype) < 100) {
    strcpy(str, chtype);
  } else {
    str = NULL;
  }
  return str;
}  

char *ipv4_htoa(uint32_t ip)
{
    static char buf[INET_ADDRSTRLEN];
    sprintf(buf, "%d.%d.%d.%d", (ip & 0xff000000) >> 24, (ip & 0x00ff0000) >> 16, (ip & 0x0000ff00) >> 8, ip & 0x000000ff);
    return buf;
}

char *ipv4_ntoa(uint32_t ip)
{
    unsigned int lip = ntohl(ip);
    return ipv4_htoa(lip);
}

int32_t ns_resolv(const char *name)
{
    struct hostent *host;

    host = gethostbyname(name);

    if (host == NULL)
    {
	return 0;
    }

    if (host->h_addrtype != AF_INET)
    {
	return 0;
    }

    return ((struct in_addr) *((struct in_addr *) host->h_addr_list[0])).s_addr;
}

void ilist_resolve(void)
{
    int i = 0;
    int j;
    char buf[255];
    char *plist = ilist;
    char *nlist;
    uint32_t ip;


    if (ilist == NULL)
    {
	return;
    }

    do
    {
	
	strncpy(buf, plist, 254);

        nlist = strstr(buf, ",");

	if (nlist != NULL)
	{
	    *nlist = 0;
	}
#ifdef HAVE_DEBUG
	if (debug) fprintf(stderr, "Ignoring host: %s", buf);
#endif
	ip = ns_resolv(buf);

	if (ip != 0)
	{
	    for (j=0; j<i; j++)
	    {
		if (ignore_ip[j] == ip)
		{
		    /* already in list - clear */
		    ip = 0;
		}
	    }
	
	    if (ip != 0)
	    {
		ignore_ip[i] = ip;
#ifdef HAVE_DEBUG
		if (debug) fprintf(stderr, " address: %s\n", ipv4_ntoa(ip));
#endif
		if (strcmp(plist, ipv4_ntoa(ip)) != 0)
		{
		    dns_ignore_lookup = TRUE;
		}
		i++;
	    }
	    else
	    {
#ifdef HAVE_DEBUG
		if (debug) fprintf(stderr, " - already in list\n");
#endif
	    }
	}
	else
	{
	    if (strstr(buf, "44.") == buf)
	    {
		encap_ignore = TRUE;
#ifdef HAVE_DEBUG
		if (debug) fprintf(stderr, " - ampr entry\n");
#endif
	    }
#ifdef HAVE_DEBUG
	    else
	    {
		if (debug) fprintf(stderr, " - invalid hostname\n");
	    }
#endif
	}

	plist = strstr(plist, ",");
	if (plist != NULL) plist++;

    } while ((i<MAXIGNORE) && (plist != NULL));

#ifdef HAVE_DEBUG
    if (debug) 
    {
	if (verbose) fprintf(stderr, "Total %d IPs in ignore lookup table.\n", i);
	if (dns_ignore_lookup) fprintf(stderr, "Hostname usage found in ignore list - will do lookups after RIP update.\n");
    }
#endif

    while (i<MAXIGNORE)
    {
	ignore_ip[i] = 0;
	i++;
    }
}

uint32_t getip(const char *dev)
{
    struct ifreq ifr;
    int res;

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) return 0;

    memset(&ifr, 0, sizeof(struct ifreq));
    strcpy(ifr.ifr_name, dev);
    res = ioctl(sockfd, SIOCGIFADDR, &ifr);
    close(sockfd);
    if (res < 0) return 0;
    return ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr.s_addr;
}

void set_multicast(int sockfd, const char *dev)
{
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(struct ifreq));
    strcpy(ifr.ifr_name, dev);

    if (ioctl(sockfd, SIOCGIFFLAGS, &ifr)< 0)
    {
	return;
    }

    ifr.ifr_flags |= IFF_MULTICAST;

    ioctl(sockfd, SIOCSIFFLAGS, &ifr);

    return;
}


char *ipv4_ntoa_encap(uint32_t lip)
{
    static char buf[INET_ADDRSTRLEN];
    char *p;
    sprintf(buf, "%d.%d", (lip & 0xff000000) >> 24, (lip & 0x00ff0000) >> 16);
    if ((((lip & 0x0000ff00) >> 8) != 0) || ((lip & 0x000000ff) != 0))
    {
	p = &buf[strlen(buf)];
	sprintf(p, ".%d", (lip & 0x0000ff00) >> 8);
	if ((lip & 0x000000ff) != 0)
	{
	    p = &buf[strlen(buf)];
	    sprintf(p, ".%d", lip & 0x000000ff);
	}
    }
    return buf;
}

char *idx_encap(int idx)
{
    static char *buf;
    uint32_t lip = ntohl(routes[idx].address);
    buf=ipv4_ntoa_encap(lip);
    return buf;
}

void set_rt_table(char *arg)
{
    FILE *rtf;
    char buffer[255];
    char sbuffer[255];
    char *p;
    int i;

    if (NULL == arg)
    {
	nrtable =  RT_TABLE_MAIN;
#ifdef HAVE_DEBUG
	if (debug) fprintf(stderr, "Using routing table 'main' (%d).\n", nrtable);
#endif
    }
    else if (strcmp("default", arg) == 0)
    {
	nrtable =  RT_TABLE_DEFAULT;
#ifdef HAVE_DEBUG
	if (debug) fprintf(stderr, "Using routing table 'default' (%d).\n", nrtable);
#endif
    }
    else if (strcmp("main", arg) == 0)
    {
	nrtable =  RT_TABLE_MAIN;
#ifdef HAVE_DEBUG
	if (debug) fprintf(stderr, "Using routing table 'main' (%d).\n", nrtable);
#endif
    }
    else if (strcmp("local", arg) == 0)
    {
	nrtable =  RT_TABLE_LOCAL;
#ifdef HAVE_DEBUG
	if (debug) fprintf(stderr, "Using routing table 'local' (%d).\n", nrtable);
#endif
    }
    else
    {
	/* check for a number */
	for (i=0; i<strlen(arg); i++)
	{
	    if (!isdigit(arg[i]))
		break;
	}

	if (i==strlen(arg)) /* we are all digits */
	{
	    if (1 != sscanf(arg, "%d", &nrtable))
	    {
		/* fallback */
		nrtable = RT_TABLE_MAIN;
#ifdef HAVE_DEBUG
		if (debug) fprintf(stderr, "Can not find routing table '%s'. Assuming table 'main' (%d)", table, nrtable);
#endif
	    }
#ifdef HAVE_DEBUG
	    if (debug) fprintf(stderr, "Using routing table (%d).\n", nrtable);
#endif
	}
	else /* we have a table name  */
	{
	    rtf = fopen(RTAB_FILE, "r");
	    if (NULL == rtf)
	    {
#ifdef HAVE_DEBUG
		if (debug) fprintf(stderr, "Can not open routing table file '%s'. Assuming table main (254)\n", RTAB_FILE);
#endif
		nrtable = RT_TABLE_MAIN;
	    }

	    while (fgets(buffer, 255, rtf) != NULL)
	    {
		if ((buffer[0]!='#') && (p = strstr(buffer, table)) != NULL)
		{
		    if (2 == sscanf(buffer, "%d %s", &nrtable, (char *)&sbuffer))
		    {
			if (0 == strcmp(table, sbuffer))
			{
#ifdef HAVE_DEBUG
			    if (debug) fprintf(stderr, "Using routing table '%s' (%d).\n", table, nrtable);
#endif
			    return;
			}
		    }
		}
		p = NULL;
		continue;
	    }
	    nrtable = RT_TABLE_MAIN;
#ifdef HAVE_DEBUG
	    if (debug) fprintf(stderr, "Can not find routing table %s. Assuming table 'main' (%d)", table, nrtable);
#endif
	    fclose (rtf);
	}
    }
}

void detect_myips(void)
{
    int i, j;
    uint32_t ipaddr;

    struct if_nameindex *names;

    for (i=0; i<MYIPSIZE; i++) myips[i] = 0;

    names = if_nameindex();

    if (NULL == names)
    {
	return;
    }

    i = 0;
    while ((names[i].if_index != 0) && (names[i].if_name != NULL) && (i<MYIPSIZE))
    {
	ipaddr = getip(names[i].if_name);

#ifdef HAVE_DEBUG
	if (debug && verbose) fprintf(stderr, "Interface detected: %s, IP: %s\n", names[i].if_name, ipv4_ntoa(ipaddr));
#endif

	if (strcmp(names[i].if_name, tunif) == 0)
	{
	    tunidx = names[i].if_index;
#ifdef HAVE_DEBUG
	    if (debug && verbose) fprintf(stderr, "Assigned tunnel interface index: %u\n", tunidx);
#endif
	}

	/* check if address not already there */
	for (j=0; j<MYIPSIZE; j++)
	{
	    if ((myips[j] == ipaddr) || (0 == myips[j])) break;
	}
	if (MYIPSIZE != j) myips[j] = ipaddr;

	i++;
    }

    if_freenameindex(names);

#ifdef HAVE_DEBUG
    if (debug && verbose)
    {
	fprintf(stderr, "Local IPs:\n");
        for (i=0; i<MYIPSIZE; i++)
	{
	    if (0 == myips[i]) break;
	    fprintf(stderr, "   %s\n", ipv4_ntoa(myips[i]));
	}
    }
#endif
}

int check_ignore(uint32_t ip)
{
	int i;
	
	/* check for a local interface match */
	for (i=0; i<MYIPSIZE; i++)
	{
	    if (0 == myips[i]) break;
	    if (ip == myips[i]) return TRUE;
	}

	/* check for a local interface match */
	for (i=0; i<MAXIGNORE; i++)
	{
	    if (0 == ignore_ip[i]) break;
	    if (ip == ignore_ip[i]) return TRUE;
	}

	/* valid IP */
	return FALSE;
};

int check_ignore_encap(uint32_t ip, uint32_t mask)
{
    char *plist = ilist;
    char *nlist;
    char buf[255];
    char encb[INET_ADDRSTRLEN + 3];
    char nb[INET_ADDRSTRLEN + 3];

    sprintf(encb, "%s/%d", ipv4_ntoa_encap(ntohl(ip)), mask);
    sprintf(nb, "%s/%d", ipv4_ntoa(ip), mask);

    if (ilist == NULL)
    {
	return FALSE;
    }

    do
    {
	strncpy(buf, plist, 254);

        nlist = strstr(buf, ",");

	if (nlist != NULL)
	{
	    *nlist = 0;
	}

	if (strcmp(buf, encb) == 0)
	{
	    return TRUE;
	}

	if (strcmp(buf, nb) == 0)
	{
	    return TRUE;
	}

	plist = strstr(plist, ",");
	if (plist != NULL) plist++;

    } while (plist != NULL);

    /* valid subnet */
    return FALSE;
}

void list_add(uint32_t address, uint32_t netmask, uint32_t nexthop)
{
    int i;

    /* find a free entry */
    for (i=0; i<RTSIZE; i++)
    {
	if (0 == routes[i].timestamp)
	{
	    routes[i].address = address;
	    routes[i].netmask = netmask;
	    routes[i].nexthop = nexthop;
	    routes[i].timestamp = time(NULL);
	    break;
	}
    }

#ifdef HAVE_DEBUG
    if (RTSIZE == i)
    {
	if (debug) fprintf(stderr, "Can not find an unused route entry.\n");
    }
#endif
}

int list_count(void)
{
    int count = 0;
    int i;

    for (i=0; i<RTSIZE; i++)
    {
	if (0 != routes[i].timestamp)
	{
	    count++;
	}
    }
    return count;
}

int list_find(uint32_t address, uint32_t netmask)
{
    int i;

    for (i=0; i<RTSIZE; i++)
    {
	if ((routes[i].address == address) && (routes[i].netmask == netmask))
	{
	    break;
	}
    }

    if (RTSIZE == i)
    {
	return -1;
    }
    else
    {
	return i;
    }
}

void list_update(uint32_t address, uint32_t netmask, uint32_t nexthop)
{
    int entry;
    entry = list_find(address, netmask);
    if (-1 != entry)
    {
	if (routes[entry].nexthop != nexthop)
	{
	    routes[entry].nexthop = nexthop;
	    updated = TRUE;
	}
	routes[entry].timestamp = time(NULL);
    }
    else
    {
	list_add(address, netmask, nexthop);
	updated = TRUE;
    }
}

void list_remove(int idx)
{
	routes[idx].address = 0;
	routes[idx].netmask = 0;
	routes[idx].nexthop = 0;
	routes[idx].timestamp = 0;

}

void list_clear(void)
{
    int i;
    for (i=0; i<RTSIZE; i++)
    {
	list_remove(i);
    }
}

void save_encap(void)
{
	int i;
	FILE *efd;
	time_t clock;

	if ((FALSE == updated) || (FALSE == save))
	{
#ifdef HAVE_DEBUG
	    if (debug && verbose) fprintf(stderr, "Saving to encap file not needed.\n");
#endif
	}
	else
	{
	    efd = fopen(RTFILE, "w+");
	    if (NULL == efd)
	    {
#ifdef HAVE_DEBUG
		if (debug) fprintf(stderr, "Can not open encap file for writing: %s\n", RTFILE);
#endif
	    }
	    else
	    {

		clock = time(NULL);

		fprintf(efd, "#\n");
		fprintf(efd, "# encap.txt file - saved by ampr-ripd (UTC) %s", asctime(gmtime(&clock)));
		fprintf(efd, "#\n");

		for (i=0; i<RTSIZE; i++)
		{
		    if (0 != routes[i].timestamp)
		    {
			fprintf(efd, "route addprivate %s", idx_encap(i));
			fprintf(efd, "/%d encap ", routes[i].netmask);
			fprintf(efd, "%s\n", ipv4_ntoa(routes[i].nexthop));
		    }
		}

		fprintf(efd, "# --EOF--\n");

		fclose(efd);
	    }
	}

	if ((NULL != syscmd) && (TRUE == updated))
	{
	    i = system(syscmd);
	    if ((0 != i) && debug)
	    {
		fprintf(stderr, "Error executing \"%s\"\n", syscmd);
	    }
	}

	updated = FALSE;
}

void load_encap(void)
{
	int i;
	int count = 0;
	FILE *efd;
	char buffer[255];
	char *p;
	uint32_t b1, b2, b3, b4, nr;
	uint32_t ipaddr;
	uint32_t netmask;
	uint32_t nexthop;

	efd = fopen(RTFILE, "r");
	if (NULL == efd)
	{
#ifdef HAVE_DEBUG
		if (debug) fprintf(stderr, "Can not open encap file for reading: %s\n", RTFILE);
#endif
		return;
	}

	while (fgets(buffer, 255, efd) != NULL)
	{
		if ((buffer[0]!='#') && ((p = strstr(buffer, "addprivate ")) != NULL))
		{
		    p = &p[strlen("addprivate ")];
		    b1 = b2 = b3 = b4 = 0;
		    netmask = 0;
		    ipaddr = 0;
		    nr =sscanf(p, "%d.%d.%d.%d", &b1, &b2, &b3, &b4);
		    if (nr < 2) continue;
		    ipaddr = (b1 << 24) | (b2 << 16);
		    if (nr > 2) ipaddr |= b3 << 8;
		    if (nr > 3) ipaddr |= b4;
		    p = strstr(p, "/"); p = &p[1];
		    if (sscanf(p, "%d", &netmask) != 1) continue;
		    p = strstr(p, "encap ");
		    if (p == NULL) continue;
		    p = &p[strlen("encap ")];
		    nr =sscanf(p, "%d.%d.%d.%d", &b1, &b2, &b3, &b4);
		    if (nr < 4) continue;
		    nexthop = (b1 << 24) | (b2 << 16) | b3 << 8 | b4;
		
		    /* find a free entry */
		    for (i=0; i<RTSIZE; i++)
		    {
			if (0 == routes[i].timestamp)
			{
			    routes[i].address = htonl(ipaddr);
			    routes[i].netmask = netmask;
			    routes[i].nexthop = htonl(nexthop);
			    routes[i].timestamp = 1; /* expire at first update */
			    break;
			}
		    }

#ifdef HAVE_DEBUG
		    if (RTSIZE == i)
		    {
			if (debug) fprintf(stderr, "Can not find an unused route entry.\n");
		    }
#endif

		    count++;
		}
	}

#ifdef HAVE_DEBUG
	if (debug && verbose) fprintf(stderr, "Loaded %d entries from %s\n", count, RTFILE);
#endif

	fclose(efd);

	if (count) updated = TRUE;

}

int addattr_len(struct nlmsghdr *n, int maxlen, int type, const void *data, int alen)
{
    int len = RTA_LENGTH(alen);
    struct rtattr *rta;

    if ((NLMSG_ALIGN(n->nlmsg_len) + len) > maxlen)
    {
#ifdef HAVE_DEBUG
	if (debug) fprintf(stderr, "Max allowed length exceeded during NLMSG assembly.\n");
#endif
	return -1;
    }
    rta = NLMSG_TAIL(n);
    rta->rta_type = type;
    rta->rta_len = len;
    memcpy(RTA_DATA(rta), data, len);
    n->nlmsg_len = NLMSG_ALIGN(n->nlmsg_len) + len;
    return 0;
}

int rta_addattr_len(struct rtattr *rta, int maxlen, int type, const void *data, int alen)
{
    struct rtattr *subrta;
    int len = RTA_LENGTH(alen);
    if ((RTA_ALIGN(rta->rta_len) + RTA_ALIGN(len)) > maxlen)
    {
#ifdef HAVE_DEBUG
	if (debug) fprintf(stderr, "Max allowed length exceeded during sub-RTA assembly.\n");
#endif
	return -1;
    }

    subrta = (struct rtattr *)(((void *)rta) + RTA_ALIGN(rta->rta_len));
    subrta->rta_type = type;
    subrta->rta_len = len;
    memcpy(RTA_DATA(subrta), data, alen);
    rta->rta_len = NLMSG_ALIGN(rta->rta_len) + RTA_ALIGN(len);
    return 0;
}

#ifdef HAVE_DEBUG
#ifdef NL_DEBUG
void nl_debug(void *msg, int len)
{
    struct rtattr *rtattr;
    struct nlmsghdr *rh;
    struct rtmsg *rm;
    int i;
    unsigned char *c;
    char tempch[100];
    struct nlmsgerr *rerr;

    if (debug && verbose)
    {
	for (rh = (struct nlmsghdr *)msg; NLMSG_OK(rh, len); rh = NLMSG_NEXT(rh, len))
	{
	
	    if (NLMSG_ERROR == rh->nlmsg_type)
	    {
                rerr = (struct nlmsgerr*)NLMSG_DATA(rh);
		fprintf(stderr, "NLMSG: error %i\n",rerr->error);
	    }
	    else if (NLMSG_DONE == rh->nlmsg_type)
	    {
		fprintf(stderr, "NLMSG: done\n");
	    }
	    else
	    {
		if ((RTM_NEWROUTE != rh->nlmsg_type) && (RTM_DELROUTE != rh->nlmsg_type) && (RTM_GETROUTE != rh->nlmsg_type))
		{
		    fprintf(stderr, "NLMSG: %d\n", rh->nlmsg_type);
		
		    for (i=0; i<((struct nlmsghdr *)msg)->nlmsg_len; i++)
		    {
			c = (unsigned char *)&msg;
			fprintf(stderr, "%u ", c[i]);
		    }
		    fprintf(stderr, "\n");
		}
		else
		{
		    if (RTM_NEWROUTE == rh->nlmsg_type)
		    {
			c = (unsigned char *)"request new route/route info (24)";
		    }
		    else if (RTM_DELROUTE == rh->nlmsg_type)
		    {
			c = (unsigned char *)"delete route (25)";
		    }
		    else /* RTM_GETROUTE */
		    {
			c = (unsigned char *)"get route (26)";
		    }

		    fprintf(stderr, "NLMSG: %s\n", c);
		    rm = NLMSG_DATA(rh);
		    for (rtattr = (struct rtattr *)RTM_RTA(rm); RTA_OK(rtattr, len); rtattr = RTA_NEXT(rtattr, len))
		    {
			fprintf(stderr, "RTA type: %s (%d bytes): ", rta_enum(rtattr->rta_type,tempch), rtattr->rta_len);
			for(i=0; i<(rtattr->rta_len - sizeof(struct rtattr)); i++)
			{
			    c = (unsigned char *)RTA_DATA(rtattr);
			    fprintf(stderr, "%u ", c[i]);
			}
			fprintf(stderr, "\n");
		    }
		}
	    }
	}
    }
}
#endif
#endif

uint32_t route_func(rt_actions action, uint32_t address, uint32_t netmask, uint32_t nexthop)
{

    int nlsd;
    int len;

    char nlrxbuf[4096];
    char mxbuf[256];

    struct {
	struct nlmsghdr hdr;
	struct rtmsg    rtm;
	char buf[1024];
    } req;

    struct rtattr *mxrta = (void *)mxbuf;

    struct sockaddr_nl sa;
    struct rtattr *rtattr;
    struct nlmsghdr *rh;
    struct rtmsg *rm;

    uint32_t result = 0;

    mxrta->rta_type = RTA_METRICS;
    mxrta->rta_len = RTA_LENGTH(0);

    memset(&req, 0, sizeof(req));

    memset(&sa, 0, sizeof(struct sockaddr_nl));
    sa.nl_family = AF_NETLINK;
    sa.nl_pid = getpid();
    sa.nl_groups = 0;

    req.hdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
    req.hdr.nlmsg_flags = NLM_F_REQUEST;
    req.hdr.nlmsg_seq = ++seq;
    req.hdr.nlmsg_pid = getpid();
    req.rtm.rtm_family = AF_INET;
    req.rtm.rtm_dst_len = netmask;
    req.rtm.rtm_protocol = RTPROT_AMPR;

    if (NULL == table)
    {
        req.rtm.rtm_table = RT_TABLE_MAIN;
    }
    else
    {
        req.rtm.rtm_table = nrtable;
    }

    if (ROUTE_DEL == action)
    {
        req.rtm.rtm_scope = RT_SCOPE_NOWHERE;
        req.rtm.rtm_type = RTN_UNICAST;
        req.hdr.nlmsg_type = RTM_DELROUTE;
        req.hdr.nlmsg_flags |= NLM_F_CREATE;
        result = address;
    }
    else if (ROUTE_ADD == action)
    {
	req.rtm.rtm_flags |= RTNH_F_ONLINK;
	req.rtm.rtm_type = RTN_UNICAST;
	req.hdr.nlmsg_type = RTM_NEWROUTE;
	req.hdr.nlmsg_flags |= NLM_F_CREATE;
	result = nexthop;
    }
    else
    {
	req.hdr.nlmsg_type = RTM_GETROUTE;
	req.rtm.rtm_scope = RT_SCOPE_UNIVERSE;
    }

    addattr32(&req.hdr, sizeof(req), RTA_DST, address);

    if (ROUTE_ADD == action)
    {
	if (0 != nexthop) addattr32(&req.hdr, sizeof(req), RTA_GATEWAY, nexthop); /* gateway */
	addattr32(&req.hdr, sizeof(req), RTA_OIF, tunidx); /* dev */
	if (rmetric>0)
	{
	    addattr32(&req.hdr, sizeof(req), RTA_PRIORITY, rmetric); /* metrics */
	}
	if (rwindow>0)
	{
	    rta_addattr32(mxrta, sizeof(mxbuf), RTAX_WINDOW, rwindow);
	    addattr_len(&req.hdr, sizeof(req), RTA_METRICS, RTA_DATA(mxrta), RTA_PAYLOAD(mxrta));
	}
    }

    if ((nlsd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE)) < 0)
    {
#ifdef HAVE_DEBUG
	if (debug) fprintf(stderr, "Can not open netlink socket.\n");
#endif
	return 0;
    }

    if (bind(nlsd, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
#ifdef HAVE_DEBUG
        if (debug) fprintf(stderr, "Can not bind to netlink socket.\n");
#endif
	return 0;
    }
#ifdef HAVE_DEBUG
#ifdef NL_DEBUG
    if (debug && verbose) fprintf(stderr, "NL sending request.\n");
    nl_debug(&req, req.hdr.nlmsg_len);
#endif
#endif
    if (send(nlsd, &req, req.hdr.nlmsg_len, 0) < 0)
    {
#ifdef HAVE_DEBUG
	if (debug) fprintf(stderr, "Can not talk to rtnetlink.\n");
#endif
    }

    if ((len = recv(nlsd, nlrxbuf, sizeof(nlrxbuf), MSG_DONTWAIT|MSG_PEEK)) > 0)
    {
#ifdef HAVE_DEBUG
#ifdef NL_DEBUG
	if (debug && verbose) fprintf(stderr, "NL response received.\n");
	nl_debug(nlrxbuf, len);
#endif
#endif
	if (ROUTE_GET == action)
	{
	    /* parse response for ROUTE_GET */
	    for (rh = (struct nlmsghdr *)nlrxbuf; NLMSG_OK(rh, len); rh = NLMSG_NEXT(rh, len))
	    {
		if (rh->nlmsg_type == 24) /* route info resp */
		{
		    rm = NLMSG_DATA(rh);
		    for (rtattr = (struct rtattr *)RTM_RTA(rm); RTA_OK(rtattr, len); rtattr = RTA_NEXT(rtattr, len))
		    {
			if (RTA_GATEWAY == rtattr->rta_type)
			{
			    result = *((uint32_t *)RTA_DATA(rtattr));
			}
		    }
		}
		else if (NLMSG_ERROR == rh->nlmsg_type)
		{
		    result = 0;
		}
	    }
	}
    }
    close(nlsd);
    return result;
}

void route_update(uint32_t address, uint32_t netmask, uint32_t nexthop)
{
	if (route_func(ROUTE_GET, address, netmask, 0) != nexthop)
	{
	    route_func(ROUTE_DEL, address, netmask, 0); /* fails if route does not exist - no problem */
	    if (route_func(ROUTE_ADD, address, netmask, nexthop) == 0)
	    {
#ifdef HAVE_DEBUG
		if (debug)
		{
		    fprintf(stderr, "Failed to set route %s/%d via ", ipv4_ntoa(address), netmask);
		    fprintf(stderr, "%s on dev %s. ", ipv4_ntoa(nexthop), tunif);
		}
#endif
	    }
	}
}

void route_delete_all(void)
{
	int i;

#ifdef HAVE_DEBUG
	if (debug) fprintf(stderr, "Clearing routes (%d).\n", list_count());
#endif

	for(i=0; i<RTSIZE; i++)
	{
		if (0 != routes[i].timestamp)
		{
			route_func(ROUTE_DEL, routes[i].address, routes[i].netmask, 0);
		}
	}

}

void route_set_all(void)
{
	int i;
	
#ifdef HAVE_DEBUG
	if (debug) fprintf(stderr, "Setting routes (%d).\n", list_count());
#endif

	for(i=0; i<RTSIZE; i++)
	{
		if (0 != routes[i].timestamp)
		{
			route_update(routes[i].address, routes[i].netmask, routes[i].nexthop);
		}
	}

	if ((NULL != syscmd) && updated)
	{
	    i = system(syscmd);
	    if ((0 != i) && debug)
	    {
		fprintf(stderr, "Error executing \"%s\"\n", syscmd);
	    }
	}

	updated = FALSE;
}

int process_auth(char *buf, int len)
{
	rip_auth *auth = (rip_auth *)buf;

	if (auth->auth != 0xFFFF)
	{
#ifdef HAVE_DEBUG
		if (debug) fprintf(stderr, "Password auth requested but no password found in first RIPv2 message.\n");
#endif
		return -1;
	}
	if (ntohs(auth->type) != RIP_AUTH_PASSWD)
	{
#ifdef HAVE_DEBUG
		if (debug) fprintf(stderr, "Unsupported authentication type %d.\n", ntohs(auth->type));
#endif
		return -1;
	}

	if (strcmp((char *)auth->pass, passwd) != 0)
	{
#ifdef HAVE_DEBUG
		if (debug) fprintf(stderr, "Invalid password.\n");
#endif
		return -1;
	}

	if (ntohs(auth->type) == RIP_AUTH_PASSWD)
	{
		if (debug) fprintf(stderr, "Simple password: %s\n", auth->pass);
	}

	return 0;
}

void process_entry(char *buf)
{
	rip_entry *rip = (rip_entry *)buf;

	
	if (ntohs(rip->af) != RIP_AF_INET)
	{
#ifdef HAVE_DEBUG
		if (debug && verbose) fprintf(stderr, "Unsupported address family %d.\n", ntohs(rip->af));
#endif
		return;
	}

	unsigned int mask = 1;
	unsigned int netmask = 0;
	int i;

	for (i=0; i<32; i++)
	{
	    if (rip->mask & mask)
	    {
		netmask++;
	    }
	    mask <<= 1;
	}

#ifdef HAVE_DEBUG
	if (debug && verbose)
	{
		fprintf(stderr, "Entry: address %s/%d ", ipv4_ntoa(rip->address), netmask);
		fprintf(stderr, "nexthop %s ", ipv4_ntoa(rip->nexthop));
		fprintf(stderr, "metric %d", ntohl(rip->metric));
	}
#endif

	/* drop 44.0.0.1 */
	if (rip->address == inet_addr("44.0.0.1"))
	{
#ifdef HAVE_DEBUG
	    if (debug && verbose) fprintf(stderr, " - rejected\n");
#endif
	    return;
	}

	/* validate and update the route */

	/* drop routes with gw in their own subnet */
	if ((rip->address << (32 - netmask)) == (rip->nexthop << (32 - netmask)))
	{
#ifdef HAVE_DEBUG
	    if (debug && verbose) fprintf(stderr, " - rejected\n");
#endif
	    return;
	}

	/* remove if unreachable and in list */
	if (ntohl(rip->metric) > 14)
	{
#ifdef HAVE_DEBUG
		if (debug && verbose) fprintf(stderr, " - unreacheable");
#endif
		if ((i = list_find(rip->address, netmask)) != -1)
		{
			route_func(ROUTE_DEL, rip->address, netmask, 0);
			list_remove(i);
#ifdef HAVE_DEBUG
			if (debug && verbose) fprintf(stderr, ", removed from list");
#endif
		}
#ifdef HAVE_DEBUG
		if (debug && verbose) fprintf(stderr, ".\n");
#endif
		return;
	}

	/* check if in ignore list */
	if (check_ignore(rip->nexthop) || check_ignore_encap(rip->address, netmask))
	{
#ifdef HAVE_DEBUG
		if (debug && verbose) fprintf(stderr, " - in ignore list, rejected\n");
#endif
		return;
	}

#ifdef HAVE_DEBUG
	if (debug && verbose) fprintf(stderr, "\n");
#endif

	/* update routes */
	route_update(rip->address, netmask, rip->nexthop);
	list_update(rip->address, netmask, rip->nexthop);
}


int process_message(char *buf, int len)
{
	rip_header *hdr;

	if (len < RIP_HDR_LEN + RIP_ENTRY_LEN)
	{
#ifdef HAVE_DEBUG
		if (debug) fprintf(stderr, "RIP packet to short: %d bytes", len);
#endif
		return -1;
	}
	if (len > RIP_HDR_LEN + RIP_ENTRY_LEN * 25)
	{
#ifdef HAVE_DEBUG
		if (debug) fprintf(stderr, "RIP packet to long: %d bytes", len);
#endif
		return -1;
	}
	if ((len - RIP_HDR_LEN)%RIP_ENTRY_LEN != 0)
	{
#ifdef HAVE_DEBUG
		if (debug) fprintf(stderr, "RIP invalid packet length: %d bytes", len);
#endif
		return -1;
	}

	/* packet seems plausible, process header */

	hdr = (rip_header *)buf;

#ifdef HAVE_DEBUG
	if (debug) fprintf(stderr, "RIP len %d header version %d, Command %d (%s)\n", len, hdr->version, hdr->command, rip_pcmd(hdr->command));
#endif

	if (hdr->command != RIP_CMD_RESPONSE)
	{
#ifdef HAVE_DEBUG
		if (debug) fprintf(stderr, "Ignored non-response packet\n");
#endif
		return -1;
	}

	if (hdr->version != 2)
	{
#ifdef HAVE_DEBUG
		if (debug) fprintf(stderr, "Ignored RIP version %d packet (only accept version 2).\n", hdr->version);
#endif
		return -1;
	}

	if (hdr->zeros)
	{
#ifdef HAVE_DEBUG
		if (debug) fprintf(stderr, "Ignored packet: zero bytes are not zero.\n");
#endif
		return -1;
	}

	/* header is valid, process content */

	buf += RIP_HDR_LEN;
	len -= RIP_HDR_LEN;

	/* check password if defined */

	if (-1 == process_auth(buf, len))
	{
		return -1;
	}

#ifdef HAVE_DEBUG
	if (debug) fprintf(stderr, "Simple password authentication successful.\n");
#endif
	
	buf += RIP_ENTRY_LEN;
	len -= RIP_ENTRY_LEN;

	/* simple auth ok */

	if (len == 0)
	{
#ifdef HAVE_DEBUG
		if (debug) fprintf(stderr, "No routing entries in this packet.\n");
#endif
		return -1;
	}

	/* we have some entries */

#ifdef HAVE_DEBUG
	if (debug) fprintf(stderr, "Processing RIPv2 packet, %d entries ", len/RIP_ENTRY_LEN);
	if (debug && verbose) fprintf(stderr, "\n");
#endif

	while (len >= RIP_ENTRY_LEN)
	{
		process_entry(buf);
		buf += RIP_ENTRY_LEN;
		len -= RIP_ENTRY_LEN;
	}

#ifdef HAVE_DEBUG
	if (debug) fprintf(stderr, "(total %d entries).\n", list_count());
#endif

	/* schedule a route expire check in 30 sec - we do this only if we have route reception */
	/* else we will keep the routes because there are no updates sources available!         */
	update_encap = TRUE;
	alarm(30);

	return 0;
}

static void on_term(int sig)
{
#ifdef HAVE_DEBUG
	if (debug && verbose) fprintf(stderr, "SIGTERM/SIGKILL received.\n");
#endif
	close(fwsd);
	close(tunsd);
	route_delete_all();
	exit(0); 
}

static void on_alarm(int sig)
{
	int i;
	int count = 0;

	struct sockaddr_in sin;
	rip_packet rp;
	int rip_nr;
	int route_nr;
	int size;

	if (TRUE == update_encap)
	{
	    update_encap = FALSE;

#ifdef HAVE_DEBUG
	    if (debug)
	    {
		fprintf(stderr, "SIGALRM received.\n");
		fprintf(stderr, "Checking for expired routes.\n");
	    }
#endif

	    /* recheck for dynamic ignore list entries if hostnames are in use */
	    if (dns_ignore_lookup)
	    {
		ilist_resolve();
	    }

	    /* check route timestamp and remove expired routes */
	    for(i=0; i<RTSIZE; i++)
	    {
		if ((0 != routes[i].timestamp) && ((routes[i].timestamp + EXPTIME) < time(NULL)))
		{
			route_func(ROUTE_DEL, routes[i].address, routes[i].netmask, 0);
			list_remove(i);
			count++;
			updated = TRUE;
		}
	    }

#ifdef HAVE_DEBUG
	    if (debug)
	    {
		fprintf(stderr, "Routes expired: %d.\n", count);
		fprintf(stderr, "Saving routes to disk.\n");
	    }
#endif

	    save_encap();

#ifdef HAVE_DEBUG
	    if (debug) fprintf(stderr, "(total %d entries).\n", list_count());
#endif
	}

	count = list_count();

	if ((NULL != fwif) && (count > 0))
	{
#ifdef HAVE_DEBUG
	    if (debug) fprintf(stderr, "Sending local RIP update.\n");
#endif
	    rip_nr = 0;
	    route_nr = 0;

	    memset((char *)&sin, 0, sizeof(sin));
	    sin.sin_family = PF_INET;
	    sin.sin_addr.s_addr = inet_addr(fwdest); 
	    sin.sin_port = htons(IPPORT_ROUTESERVER);

	    memset(&rp, 0, sizeof(rip_packet));
	    rp.header.version = 2;
	    rp.header.command = RIP_CMD_RESPONSE;

	    while ((rip_nr < count) && (route_nr < RTSIZE))
	    {
		size = 0;
		for (i = 0; i < 25; i++)
		{
		    while ((0 == routes[route_nr].address) && (route_nr < RTSIZE)) route_nr++;
		    if (route_nr == RTSIZE)
		    {
			break;
		    }

		    if  (rip_nr < count)
		    {
			rp.entries[i].af = htons(RIP_AF_INET);
			rp.entries[i].rtag = htons(44);
			rp.entries[i].address = routes[route_nr].address;
			rp.entries[i].mask = htonl(0xFFFFFFFFl << (32 - routes[route_nr].netmask));
			rp.entries[i].nexthop = routes[route_nr].nexthop;
			rp.entries[i].metric = htonl(2);
			rip_nr ++;
			route_nr++;
			size++;
		    }
		    else
		    {
			break;
		    }
		}

		sendto(fwsd, &rp, sizeof(rip_header) + size * sizeof(rip_entry), 0, (struct sockaddr *)&sin, sizeof(sin));

	    }

	}

	/* resend local RIP data every 29 sec - this will prevent overlapping at 5 min with the AMPR RIP update */
	alarm(29);
}

static void on_hup(int sig)
{
#ifdef HAVE_DEBUG
	if (debug) fprintf(stderr, "SIGHUP received!\n");
#endif
	route_delete_all();
	list_clear();
	updated = TRUE;
}

int main(int argc, char **argv)
{
	int p;

	struct sockaddr_in sin;
	
	char databuf[BUFFERSIZE];
	char *pload;
	int len, plen;
	int lval;

	while ((p = getopt(argc, argv, "dvsrh?i:a:p:t:m:w:f:e:x:")) != -1)
	{
		switch (p)
		{
		case 'd':
			debug = TRUE;
			break;
		case 'v':
			verbose = TRUE;
			break;
		case 's':
			save = TRUE;
			break;
		case 'r':
			/* ignore */
			break;
		case 'i':
			tunif = optarg;
			break;
		case 'a':
			ilist = optarg;
			break;
		case 'p':
			passwd = optarg;
			break;
		case 't':
			table = optarg;
			break;
		case 'm':
			if (sscanf(optarg, "%d", &lval)==1)
			{
			    rmetric = (uint32_t) lval;
			}
			break;
		case 'w':
			if (sscanf(optarg, "%d", &lval)==1)
			{
			    rwindow = (uint32_t) lval;
			}
			break;
		case 'f':
			fwif = optarg;
			break;
		case 'e':
			fwdest = optarg;
			break;
		case 'x':
			syscmd = optarg;
			break;
		case ':':
		case 'h':
		case '?':
			fprintf(stderr, "%s", usage_string);
			return 1;
		}
	}

	if (debug && verbose)
	{
		fprintf(stderr, "Using metric %d for routes.\n", rmetric);
		fprintf(stderr, "Using TCP window %d for routes.\n", rwindow);
		if (NULL != syscmd) fprintf(stderr, "Executing system command \"%s\" on encap load/save\n", syscmd);
#ifdef HAVE_DEBUG
		if (NULL !=ilist) fprintf(stderr, "Ignore list: %s\n", ilist);
#endif
	}

	ilist_resolve();

	set_rt_table(table);

	list_clear();
	load_encap();

	seq = time(NULL);

#ifdef HAVE_DEBUG
	if (debug && verbose)
	{
		fprintf(stderr, "Max list size: %d entries\n", RTSIZE);
	}
#endif

	tunaddr = getip(tunif);

#ifdef HAVE_DEBUG
	if (debug) fprintf(stderr, "Detected tunnel interface address: %s\n", ipv4_ntoa(tunaddr));
#endif

	detect_myips();

	route_set_all();

	/* create multicast listen socket on tunnel */

#ifdef HAVE_DEBUG
	if (debug) fprintf(stderr, "Creating RIP UDP listening socket.\n");
#endif

	if ((tunsd = socket(PF_INET, SOCK_RAW, 4)) < 0)
	{
	    PERROR("Raw socket");
		return 1;
	}

	if (NULL != fwif)
	{
		/* create the forward socket */
#ifdef HAVE_DEBUG
		if (debug && verbose) fprintf(stderr, "Setting up forwarding interface.\n");
#endif
		if ((fwsd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP)) < 0)
		{
			PERROR("Forward socket");
			close(tunsd);
			return 1;
		}

		if (setsockopt(fwsd, SOL_SOCKET, SO_BINDTODEVICE, fwif, strlen(fwif)) < 0)
		{
			PERROR("Tunnel socket: Setting SO_BINDTODEVICE");
			close(fwsd);
			close(tunsd);
			return 1;
		}

		memset((char *)&sin, 0, sizeof(sin));
		sin.sin_family = PF_INET;
		sin.sin_addr.s_addr = INADDR_ANY;
		sin.sin_port = htons(IPPORT_ROUTESERVER);
		
		if (bind(fwsd, (struct sockaddr *)&sin, sizeof(sin)))
		{
			PERROR("Forward socket: Bind");
			close(fwsd);
			close(tunsd);
			return 1;
		}
	}

	/* networking up and running */

	if (FALSE == debug)
	{
		/* try to become a daemon */
		
		if (daemon(0,verbose)<0)
		{
		    PERROR("Can not become a daemon");
		}
	}

	signal(SIGTERM, on_term);
	signal(SIGKILL, on_term);
	signal(SIGHUP, on_hup);
	signal(SIGALRM, on_alarm);

	alarm(30);

	/* daemon or debug */

	if (debug) fprintf(stderr, "Waiting for RIPv2 broadcasts...\n");


	while (1)
	{
		if ((len = read(tunsd, databuf, BUFFERSIZE)) < 0)
		{
			if (debug) fprintf(stderr, "Socket read error.\n");
		}
		else
		{
			if (len >= 48 + (RIP_HDR_LEN + RIP_ENTRY_LEN))
			{
				struct iphdr *iph = (struct iphdr *)(databuf + 20);
				struct udphdr *udh = (struct udphdr *)(databuf + 40);
			
				if ((iph->daddr == inet_addr("224.0.0.9")) &&
				    (iph->saddr == inet_addr("44.0.0.1")) &&
				    (udh->dest == htons(IPPORT_ROUTESERVER)) &&
				    (udh->source == htons(IPPORT_ROUTESERVER)))
				{
				    pload = &databuf[48];
				    plen = len - 48;
				}
				else
				{
				    continue;
				}
			}
			else
			{
				continue;
			}
			
			process_message(pload, plen);
			
		}
	}

	/* we never reach this */
	return 0; 
}
