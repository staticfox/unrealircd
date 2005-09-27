/************************************************************************
 * IRC - Internet Relay Chat, random.c
 * (C) 2005 Bram Matthys (Syzop) and the UnrealIRCd Team
 *
 * See file AUTHORS in IRC package for additional names of
 * the programmers. 
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 1, or (at your option)
 * any later version.
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
 */

#include "struct.h"
#include "common.h"
#include "sys.h"
#include "numeric.h"
#include "msg.h"
#include "proto.h"
#include "channel.h"
#include "version.h"
#include <time.h>
#ifdef _WIN32
#include <sys/timeb.h>
#endif
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#endif
#include <fcntl.h>
#include "h.h"

#include <res.h>

/* Forward declerations */
void unrealdns_cb_iptoname(void *arg, int status, struct hostent *he);
void unrealdns_cb_nametoip_verify(void *arg, int status, struct hostent *he);
void unrealdns_cb_nametoip_link(void *arg, int status, struct hostent *he);
void unrealdns_delasyncconnects(void);
static unsigned int unrealdns_haship(void *binaryip, int length);
static void unrealdns_addtocache(char *name, void *binaryip, int length);
static char *unrealdns_findcache_byaddr(struct IN_ADDR *addr);
struct hostent *unreal_create_hostent(char *name, struct IN_ADDR *addr);
static void unrealdns_freeandremovereq(DNSReq *r);
DNSCache *unrealdns_removecacherecord(DNSCache *c);

/* Externs */
extern void proceed_normal_client_handshake(aClient *acptr, struct hostent *he);

/* Global variables */

ares_channel resolver_channel; /**< The resolver channel. */

DNSStats dnsstats;

static DNSReq *requests = NULL; /**< Linked list of requests (pending responses). */

static DNSCache *cache_list = NULL; /**< Linked list of cache */
static DNSCache *cache_hashtbl[DNS_HASH_SIZE]; /**< Hash table of cache */

static unsigned int unrealdns_num_cache = 0; /**< # of cache entries in memory */

void init_resolver(void)
{
	if (requests)
		abort(); // should never happen
	memset(&cache_hashtbl, 0, sizeof(cache_hashtbl));
	memset(&dnsstats, 0, sizeof(dnsstats));

//	..todo..add..options..such..as..retry..and..timeout..!!!!!
	ares_init(&resolver_channel);
}

void unrealdns_addreqtolist(DNSReq *r)
{
	if (requests)
	{
		r->next = requests;
		requests->prev = r;
	}
	requests = r;
}

/** Get (and verify) the host for an incoming client.
 * - it checks the cache first, returns the host if found (and valid).
 * - if not found in cache it does ip->name and then name->ip, if both resolve
 *   to the same name it is accepted, otherwise not.
 *   We return NULL in this case and an asynchronic request is done.
 *   When done, proceed_normal_client_handshake() is called.
 */
struct hostent *unrealdns_doclient(aClient *cptr)
{
DNSReq *r;
#ifdef INET6
char ipv4[4];
#endif
static struct hostent *he;
char *cache_name, ipv6;

	cache_name = unrealdns_findcache_byaddr(&cptr->ip);
	if (cache_name)
		return unreal_create_hostent(cache_name, &cptr->ip);

	/* Create a request */
	r = MyMallocEx(sizeof(DNSReq));
	r->cptr = cptr;
	r->ipv6 = isipv6(&cptr->ip);
	unrealdns_addreqtolist(r);
	
	/* Execute it */
#ifndef INET6
	/* easy */
	ares_gethostbyaddr(resolver_channel, &cptr->ip, 4, AF_INET, unrealdns_cb_iptoname, r);
#else
	if (r->ipv6)
		ares_gethostbyaddr(resolver_channel, &cptr->ip, 16, AF_INET6, unrealdns_cb_iptoname, r);
	else {
		/* This is slightly more tricky: convert it to an IPv4 presentation and issue the request with that */
		memcpy(ipv4, ((char *)&cptr->ip) + 12, 4);
		ares_gethostbyaddr(resolver_channel, ipv4, 4, AF_INET, unrealdns_cb_iptoname, r);
	}
#endif

	return NULL;
}

/** Resolve a name to an IP, for a link block.
 */
void unrealdns_gethostbyname_link(char *name, ConfigItem_link *conf)
{
DNSReq *r;
#ifdef INET6
char ipv4[4];
#endif

	/* Create a request */
	r = MyMallocEx(sizeof(DNSReq));
	r->linkblock = conf;
	r->name = strdup(name);
#ifdef INET6
	/* We try IPv6 first, and if that fails we try IPv4 */
	r->ipv6 = 1;
#else
	r->ipv6 = 0;
#endif
	unrealdns_addreqtolist(r);
	r->name = strdup(name);
	
	/* Execute it */
#ifndef INET6
	ares_gethostbyname(resolver_channel, r->name, AF_INET, unrealdns_cb_nametoip_link, r);
#else
	ares_gethostbyname(resolver_channel, r->name, r->ipv6 ? AF_INET6 : AF_INET, unrealdns_cb_nametoip_link, r);
#endif
}

void unrealdns_cb_iptoname(void *arg, int status, struct hostent *he)
{
DNSReq *r = (DNSReq *)arg;
DNSReq *newr;
aClient *acptr = r->cptr;

	if (!acptr)
	{
		unrealdns_freeandremovereq(r);
		return; 
	}
	
	if (status != 0)
	{
		/* Failed */
		unrealdns_freeandremovereq(r);
		proceed_normal_client_handshake(acptr, NULL);
		return;
	}

	/* Good, we got a valid response, now prepare for name -> ip */
	newr = MyMallocEx(sizeof(DNSReq));
	newr->cptr = acptr;
	newr->ipv6 = r->ipv6;
	unrealdns_freeandremovereq(r);
	
#ifndef INET6
	ares_gethostbyname(resolver_channel, he->h_name, AF_INET, unrealdns_cb_nametoip_verify, newr);
#else
	ares_gethostbyname(resolver_channel, he->h_name, r->ipv6 ? AF_INET6 : AF_INET, unrealdns_cb_nametoip_verify, newr);
#endif
}

void unrealdns_cb_nametoip_verify(void *arg, int status, struct hostent *he)
{
DNSReq *r = (DNSReq *)arg;
aClient *acptr = r->cptr;
char ipv6 = r->ipv6;
int i;
struct hostent *he2;
u_int32_t ipv4_addr;

	unrealdns_freeandremovereq(r);
	
	if (!acptr)
		return;

	if ((status != 0) ||
#ifdef INET6
	    ((he->h_length != 4) && (he->h_length != 16)))
#else
	    (he->h_length != 4))
#endif
	{
		/* Failed: error code, or data length is not 4 (nor 16) */
		proceed_normal_client_handshake(acptr, NULL);
		return;
	}

	if (!r->ipv6)
#ifndef INET6
		ipv4_addr = acptr->ip.S_ADDR;
#else
		inet6_to_inet4(&acptr->ip, &ipv4_addr);
#endif

	/* Verify ip->name and name->ip mapping... */
	for (i = 0; he->h_addr_list[i]; i++)
	{
#ifndef INET6
		if ((he->h_length == 4) && !memcmp(he->h_addr_list[i], &ipv4_addr, 4))
			break;
#else
		if (r->ipv6)
		{
			if ((he->h_length == 16) && !memcmp(he->h_addr_list[i], &acptr->ip, 16))
				break;
		} else {
			if ((he->h_length == 4) && !memcmp(he->h_addr_list[i], &ipv4_addr, 4))
				break;
		}
#endif
	}

	if (he->h_addr_list[i])
	{
		/* Entry was found, verified, and can be added to cache */
		unrealdns_addtocache(he->h_name, &acptr->ip, sizeof(acptr->ip));
	}
	
	if (acptr)
	{
		/* Always called, because the IP<->name mapping gets verified again
		 * (plus some 'restricted hostname chars'-stuff is done ;).
		 */
		he2 = unreal_create_hostent(he->h_name, &acptr->ip);
		proceed_normal_client_handshake(acptr, he2);
	}
}

void unrealdns_cb_nametoip_link(void *arg, int status, struct hostent *he)
{
DNSReq *r = (DNSReq *)arg;
int n;
struct hostent *he2;

	if (!r->linkblock)
		return; /* Possible if deleted due to rehash async removal */

	if (status != 0)
	{
#ifdef INET6
		if (r->ipv6)
		{
			/* Retry for IPv4... */
			r->ipv6 = 0;
			ares_gethostbyname(resolver_channel, r->name, AF_INET, unrealdns_cb_nametoip_link, r);
			return;
		}
#else
		/* fatal */
		sendto_realops("Unable to resolve '%s'", r->name);
		unrealdns_freeandremovereq(r);
		return;
#endif
	}

#ifdef INET6
	if (((he->h_length != 4) && (he->h_length != 16)) || !he->h_addr_list[0])
#else
	if ((he->h_length != 4) || !he->h_addr_list[0])
#endif
	{
		/* Illegal response -- fatal */
		sendto_realops("Unable to resolve hostname '%s', when trying to connect to server %s.",
			r->name, r->linkblock->servername);
		unrealdns_freeandremovereq(r);
	}

	/* Ok, since we got here, it seems things were actually succesfull */

	/* Fill in [linkblockstruct]->ipnum */
#ifdef INET6
	if (he->h_length == 4)
		inet4_to_inet6(he->h_addr_list[0], &r->linkblock->ipnum);
	else
#endif
		memcpy(&r->linkblock->ipnum, he->h_addr_list[0], sizeof(struct IN_ADDR));

	he2 = unreal_create_hostent(he->h_name, (struct IN_ADDR *)&he->h_addr_list[0]);

	switch ((n = connect_server(r->linkblock, r->cptr, he2)))
	{
		case 0:
			sendto_realops("Connecting to %s[%s].", r->linkblock->servername, r->linkblock->hostname);
			break;
		case -1:
			sendto_realops("Couldn't connect to %s.", r->linkblock->servername);
			break;
		case -2:
			/* Should not happen since he is not NULL */
			sendto_realops("Hostname %s is unknown for server %s (!?).", r->linkblock->hostname, r->linkblock->servername);
			break;
		default:
			sendto_realops("Connection to %s failed: %s", r->linkblock->servername, STRERROR(n));
	}
	
	unrealdns_freeandremovereq(r);
	/* DONE */
}

static unsigned int unrealdns_haship(void *binaryip, int length)
{
#ifdef INET6
unsigned int alpha, beta;
#endif

	if (length == 4)
		return (*(unsigned int *)binaryip) % DNS_HASH_SIZE;

#ifndef INET6
	abort(); /* impossible */
#else	
	if (length != 16)
		abort(); /* impossible */
	
	memcpy(&alpha, (char *)binaryip + 8, sizeof(alpha));
	memcpy(&beta, (char *)binaryip + 12, sizeof(beta));

	return (alpha ^ beta) % DNS_HASH_SIZE;
#endif
}

static void unrealdns_addtocache(char *name, void *binaryip, int length)
{
unsigned int hashv;
DNSCache *c, *n;
struct IN_ADDR addr;

	dnsstats.cache_adds++;
#ifdef INET6
	/* If it was an ipv4 address, then we need to convert it to ipv4-in-ipv6 first. */
	if (length == 4)
	{
		inet4_to_inet6(binaryip, &addr);
	} else
#endif
		memcpy(&addr, binaryip, length);

	hashv = unrealdns_haship(binaryip, length);

	/* Check first if it is already present in the cache (how? don't know, but just
	 * in case, minor cpu impact, really).
	 */	
	for (c = cache_hashtbl[hashv]; c; c = c->hnext)
		if (!memcmp(&addr, &c->addr, sizeof(struct IN_ADDR)))
			break;

	if (c)
	{
		/* Entry already exists, hmmm...
		 * We COULD update it, but.. this seems suspicious?
		 * We bail out for now...
		 */
		return;
	}

	/* Remove last item, if we got too many.. */
	if (unrealdns_num_cache >= DNS_MAX_ENTRIES)
	{
		for (c = cache_list; c->next; c = c->next);
		if (!c)
			abort(); // impossible
		unrealdns_removecacherecord(c);
	}

	/* Create record */
	c = MyMallocEx(sizeof(DNSCache));
	c->name = strdup(name);
	c->expires = TStime() + DNSCACHE_TTL;
	memcpy(&c->addr, &addr, sizeof(addr));
	
	/* Add to hash table */
	if (cache_hashtbl[hashv])
	{
		cache_hashtbl[hashv]->hprev = c;
		c->hnext = cache_hashtbl[hashv];
	}
	cache_hashtbl[hashv] = c;
	
	/* Add to linked list */
	if (cache_list)
	{
		cache_list->prev = c;
		c->next = cache_list;
	}
	cache_list = c;

	unrealdns_num_cache++;
	/* DONE */
}

/** Search the cache for a confirmed ip->name and name->ip match, by address.
 * @returns The resolved hostname, or NULL if not found in cache.
 */
static char *unrealdns_findcache_byaddr(struct IN_ADDR *addr)
{
unsigned int hashv;
DNSCache *c;

	hashv = unrealdns_haship(addr, sizeof(struct IN_ADDR));
	
	for (c = cache_hashtbl[hashv]; c; c = c->hnext)
		if (!memcmp(addr, &c->addr, sizeof(struct IN_ADDR)))
		{
			dnsstats.cache_hits++;
			return c->name;
		}
	
	dnsstats.cache_misses++;
	return NULL;
}

/** Removes dns cache record from list (and frees it).
 * @returns Next entry in LINKED cache list
 */
DNSCache *unrealdns_removecacherecord(DNSCache *c)
{
unsigned int hashv;
DNSCache *next;

	/* We basically got 4 pointers to update:
	 * <previous listitem>->next
	 * <next listitem>->previous
	 * <previous hashitem>->next
	 * <next hashitem>->prev.
	 * And we need to update 'cache_list' and 'cache_hash[]' if needed.
	 */
	if (c->prev)
		c->prev->next = c->next;
	else
		cache_list = c->next; /* new list HEAD */
	
	if (c->next)
		c->next->prev = c->prev;
	
	if (c->hprev)
		c->hprev->hnext = c->hnext;
	else {
		/* new hash HEAD */
		hashv = unrealdns_haship(&c->addr, sizeof(struct IN_ADDR));
		if (cache_hashtbl[hashv] != c)
			abort(); // impossible
		cache_hashtbl[hashv] = c->hnext;
	}
	
	if (c->hnext)
		c->hnext->hprev = c->hprev;
	
	next = c->next;
	
	MyFree(c->name);
	MyFree(c);

	unrealdns_num_cache--;
	
	return next;
}

/** This regulary removes old dns records from the cache */
EVENT(unrealdns_removeoldrecords)
{
DNSCache *c, *next;

	for (c = cache_list; c; c = next)
	{
		next = c->next;
		if (c->expires < TStime())
		{
#if 0
			sendto_realops(sptr, "[Syzop/DNS] Expire: %s [%s] (%ld < %ld)",
				c->name, Inet_ia2p(&c->addr), c->expires, TStime());
#endif
			unrealdns_removecacherecord(c);
		}
	}
}

struct hostent *unreal_create_hostent(char *name, struct IN_ADDR *addr)
{
struct hostent *he;

	/* Create a hostent structure (I HATE HOSTENTS) and return it.. */
	he = MyMallocEx(sizeof(struct hostent));
	he->h_name = strdup(name);
	he->h_addrtype = AFINET;
	he->h_length = sizeof(struct IN_ADDR);
	he->h_addr_list = MyMallocEx(sizeof(char *) * 2); // alocate an array of 2 pointers
	he->h_addr_list[0] = MyMallocEx(sizeof(struct IN_ADDR));
	memcpy(he->h_addr_list[0], addr, sizeof(struct IN_ADDR));

	return he;
}

static void unrealdns_freeandremovereq(DNSReq *r)
{
	if (r->prev)
		r->prev->next = r->next;
	else
		requests = r->next; // new HEAD
	
	if (r->next)
		r->next->prev = r->prev;

	if (r->name)
		MyFree(r->name);
	MyFree(r);
}

/** Delete requests for client 'cptr'.
 * Actually we DO NOT (and should not) delete them, but simply mark them as 'dead'.
 */
void unrealdns_delreq_bycptr(aClient *cptr)
{
DNSReq *r;

	for (r = requests; r; r = r->next)
		if (r->cptr == cptr)
			r->cptr = NULL;
}

void unrealdns_delasyncconnects(void)
{
DNSReq *r;
	for (r = requests; r; r = r->next)
		if ((r->type == DNSREQ_CONNECT) || (r->type == DNSREQ_CONNECT))
			r->linkblock = NULL;
	
}

CMD_FUNC(m_dns)
{
DNSCache *c;
DNSReq *r;
char *param;

	if (!IsAnOper(sptr))
		return 0;

	if ((parc > 1) && !BadPtr(parv[1]))
		param = parv[1];
	else
		param = "";

	if (*param == 'l') /* LIST CACHE */
	{
		sendtxtnumeric(sptr, "DNS CACHE List (%u items):", unrealdns_num_cache);
		for (c = cache_list; c; c = c->next)
			sendtxtnumeric(sptr, " %s [%s]", c->name, Inet_ia2p(&c->addr));
	} else
	if (*param == 'r') /* LIST REQUESTS */
	{
		sendtxtnumeric(sptr, "DNS Request List:");
		for (r = requests; r; r = r->next)
			sendtxtnumeric(sptr, " %s",
				r->cptr ? Inet_ia2p(&r->cptr->ip) : "<client lost>");
	} else
	if (*param == 'c') /* CLEAR CACHE */
	{
		sendto_realops("%s (%s@%s) cleared the DNS cache list (/QUOTE DNS c)",
			sptr->name, sptr->user->username, sptr->user->realhost);
		
		while (cache_list)
		{
			c = cache_list->next;
			MyFree(cache_list->name);
			MyFree(cache_list);
			cache_list = c;
		}
		memset(&cache_hashtbl, 0, sizeof(cache_hashtbl));
		unrealdns_num_cache = 0;
		sendnotice(sptr, "DNS Cache has been cleared");
	} else
	if (*param == 'i') /* INFORMATION */
	{
		sendtxtnumeric(sptr, "DNS Configuration info:");
		sendtxtnumeric(sptr, " c-ares version %s",ares_version(NULL));
		// TODO: hmm... we cannot get the nameservers from c-ares, do we?
	} else /* STATISTICS */
	{
		sendto_one(sptr, ":%s %d %s :DNS CACHE Stats:",
			sptr->name, RPL_TEXT, me.name);
		sendto_one(sptr, ":%s %d %s : hits: %d",
			sptr->name, RPL_TEXT, me.name, dnsstats.cache_hits);
		sendto_one(sptr, ":%s %d %s : misses: %d (unavoidable: %d)",
			sptr->name, RPL_TEXT, me.name, dnsstats.cache_misses, dnsstats.cache_misses - dnsstats.cache_adds);
	}
	return 0;
}
