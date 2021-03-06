
#define _GNU_SOURCE

#include <sys/time.h>
#include <time.h>

#include "log.h"
#include "sha1.h"
#include "main.h"
#include "utils.h"
#include "conf.h"
#include "utils.h"
#include "results.h"
#include "net.h"
#include "values.h"
#ifdef AUTH
#include "ext-auth.h"
#endif

#include "dht.c"
#include "dht.h"
#include "kad.h"

/*
The interface that is used to interact with the DHT.
*/

/* Next time to do DHT maintenance */
static time_t g_dht_maintenance = 0;

void dht_lock_init( void ) {
#ifdef PTHREAD
	pthread_mutex_init( &gconf->dht_mutex, NULL );
#endif
}

void dht_lock( void ) {
#ifdef PTHREAD
	pthread_mutex_lock( &gconf->dht_mutex );
#endif
}

void dht_unlock( void ) {
#ifdef PTHREAD
	pthread_mutex_unlock( &gconf->dht_mutex );
#endif
}

/*
* Put an address and port into a sockaddr_storages struct.
* Both addr and port are in network byte order.
*/
void to_addr( IP *addr, const void *ip, size_t len, unsigned int port ) {
	memset( addr, '\0', sizeof(IP) );

	if( len == 4 ) {
		IP4 *a = (IP4 *) addr;
		a->sin_family = AF_INET;
		a->sin_port = port;
		memcpy( &a->sin_addr.s_addr, ip, 4 );
	}

	if( len == 16 ) {
		IP6 *a = (IP6 *) addr;
		a->sin6_family = AF_INET6;
		a->sin6_port = port;
		memcpy( &a->sin6_addr.s6_addr, ip, 16 );
	}
}

typedef struct {
	unsigned char addr[16];
	unsigned short port;
} dht_addr6_t;

typedef struct {
	unsigned char addr[4];
	unsigned short port;
} dht_addr4_t;

// Find result node in search or create new struct and add
// to search
static struct result_node *result_node_add(struct search *sr, 
        struct node *from_node){
    struct result_node *rn, *new;

    rn = sr->result_nodes;
    while(rn){
        if(id_cmp(from_node->id, rn->from_node->id) == 0){
            return rn;
        }
        rn = rn->next;
    }
    
    // create new
    new = calloc(1, sizeof(struct result_node));
    new->from_node = from_node;
    new->results = calloc(1, sizeof(struct results_t));
    
    //prepend to list
    new->next = sr->result_nodes;
    sr->result_nodes = new;
    rn = sr->result_nodes;
    return new;
}

    
/* Hajime
 * Kadnode originally just added the address to the result list or called
 * results_done(). For Hajime, we add all the logic for tracking the result
 * sets of each node that sends us results so we can obtain each node's
 * entire list.
 */
/* This callback is called when a search result arrives or a search completes */
void dht_callback_func( void *closure, int event, struct search *sr, 
        const void *data, size_t data_len, struct node *from_node) {
	struct results_t *results;
	IP addr;
	size_t i;
    int num_returned_results = 0, new_results = 0, new_node_results = 0;
    char buf0[257], buf1[257];
    UCHAR *info_hash = sr->id;
    struct result_node *rn;
	
    results = results_find( info_hash );
	
    //if no reults struct found, this is an announce search
    if( results == NULL ) {
        values_debug_print("No results for %s. Announce search\n", str_id(sr->id, buf0));
		return;
	}

    // otherwise lookup search
    /* Hajime
     * If the lookup is complete, log results 
     */
    if(event == DHT_EVENT_SEARCH_DONE ||
            event == DHT_EVENT_SEARCH_DONE6){
        values_debug_print("Results for %s are done\n",
            str_id(info_hash, buf0));
        results_done( results, 1 );
        result_nodes_done(sr, 1);
        return;
    }
    if(!from_node){
       values_debug_print("ERROR: no from node in dht_callback results\n");
       return;
    }

    // Find the result_node (node who sent the results) 
    // or add a new one
    rn = result_node_add(sr, from_node);

    // Add results to result set of the result node and the 
    // overall infohash search
	switch( event ) {
		case DHT_EVENT_VALUES:
			if( gconf->af == AF_INET ) {
				dht_addr4_t *data4 = (dht_addr4_t *) data;
                num_returned_results = (data_len / sizeof(dht_addr4_t));
				for( i = 0; i < num_returned_results; i++ ) {
					to_addr( &addr, &data4[i].addr, 4, data4[i].port );
					new_results += results_add_addr( results, &addr );
					new_node_results += results_add_addr( rn->results, &addr );
				}
			}
			break;
		case DHT_EVENT_VALUES6:
			if( gconf->af == AF_INET6 ) {
				dht_addr6_t *data6 = (dht_addr6_t *) data;
                num_returned_results = (data_len / sizeof(dht_addr6_t));
				for( i = 0; i < num_returned_results; i++ ) {
					to_addr( &addr, &data6[i].addr, 16, data6[i].port );
					new_results += results_add_addr( results, &addr );
					new_node_results += results_add_addr( rn->results, &addr );
				}
			}
			break;
	}
    
    // Update info for result node
    struct timeval now;
    gettimeofday(&now, NULL);
    rn->reply_time = now.tv_sec;
    rn->outstanding_requests -= 1;
    
    if(new_node_results){
        rn->num_new_results_responses += 1;
        if(rn->sequential_no_new_results_responses)
            rn->sequential_no_new_results_responses = 0;
    } else{
        rn->num_no_new_results_responses += 1;
        rn->sequential_no_new_results_responses += 1;
    }
    values_debug_print("Node %s returned %d results for infohash %s. %d new for node. "
            "%d new for hash. Node has %d new node responses, %d no new node "
            "responses(%d sequential), %d total\n", 
            str_addr(&from_node->ss, buf0), num_returned_results, 
            str_id(info_hash, buf1), new_node_results, new_results,
            rn->num_new_results_responses, rn->num_no_new_results_responses,
            rn->sequential_no_new_results_responses,
            (rn->num_new_results_responses + rn->num_no_new_results_responses));
    
    // If 3x as many no new result responses as new responses, with at least
    //  and as many sequential as new responses, done
    if(rn->num_no_new_results_responses >= (rn->num_new_results_responses * 3)){
        values_debug_print("%s is done\n", str_addr(&from_node->ss, buf0));
        rn->done = 1;
        return;
    }

    // Otherwise send another query
    else{
        int ret = result_node_send_get_peers(sr, rn);
        if(ret)
            values_debug_print("sent another query to %s\n", str_addr(&from_node->ss, buf0));
    }
}

/*
* Lookup in values we announce ourselves.
* Useful for networks of only one node, also faster.
*/
void kad_lookup_local_values( struct results_t *results ) {
	char addrbuf[FULL_ADDSTRLEN+1];
	struct value_t* value;
	IP addr;

	/* 127.0.0.1 */
	unsigned int inaddr_loopback = htonl( INADDR_LOOPBACK );

	value = values_find( results->id );
	if( value ) {
		if( gconf->af == AF_INET6 ) {
			to_addr( &addr, &in6addr_loopback, 16, htons( value->port ) ); // ::1
		} else {
			to_addr( &addr, &inaddr_loopback, 4, htons( value->port ) ); // 127.0.0.1
		}
		log_debug( "KAD: Address found in local values: %s\n", str_addr( &addr, addrbuf ) );
		results_add_addr( results, &addr );
	}
}

/* Handle incoming packets and pass them to the DHT code */
void dht_handler( int rc, int sock ) {
	UCHAR buf[1500];
	IP from;
	socklen_t fromlen;
	time_t time_wait = 0;


	if( rc > 0 ) {
		/* Check which socket received the data */
		fromlen = sizeof(from);
		rc = recvfrom( sock, buf, sizeof(buf) - 1, 0, (struct sockaddr*) &from, &fromlen );

		if( rc <= 0 || rc >= sizeof(buf) ) {
			goto end;
		}

		/* The DHT code expects the message to be null-terminated. */
		buf[rc] = '\0';

#ifdef AUTH
		/* Hook up AUTH extension on the DHT socket */
		if( auth_handle_challenges( sock, buf, rc, &from ) == 0 ) {
			return;
		}
#endif

		/* Handle incoming data */
		dht_lock();
		rc = dht_periodic( buf, rc, (struct sockaddr*) &from, fromlen, &time_wait, dht_callback_func, NULL );
		dht_unlock();

		if( rc < 0 && errno != EINTR ) {
			if( rc == EINVAL || rc == EFAULT ) {
				log_err( "KAD: Error calling dht_periodic." );
			}
			g_dht_maintenance = time_now_sec() + 1;
		} else {
			g_dht_maintenance = time_now_sec() + time_wait;
		}
	} else if( g_dht_maintenance <= time_now_sec() ) {
		/* Do a maintenance call */
		dht_lock();
		rc = dht_periodic( NULL, 0, NULL, 0, &time_wait, dht_callback_func, NULL );
		dht_unlock();

		/* Wait for the next maintenance call */
		g_dht_maintenance = time_now_sec() + time_wait;
		log_debug( "KAD: Next maintenance call in %u seconds.", (unsigned int) time_wait );
	} else {
		rc = 0;
	}

	if( rc < 0 ) {
		if( errno == EINTR ) {
			goto end;
		} else if( rc == EINVAL || rc == EFAULT ) {
			log_err( "KAD: Error using select: %s", strerror( errno ) );
			goto end;
		} else {
			g_dht_maintenance = time_now_sec() + 1;
		}
	}

	end:;
#ifdef AUTH
	auth_send_challenges( sock );
#endif
}

/*
* Kademlia needs dht_blacklisted/dht_hash/dht_random_bytes functions to be present.
*/

int dht_blacklisted( const struct sockaddr *sa, int salen ) {
	return 0;
}

/* Hashing for the DHT - implementation does not matter for interoperability */
void dht_hash( void *hash_return, int hash_size,
		const void *v1, int len1,
		const void *v2, int len2,
		const void *v3, int len3 ) {
	SHA1_CTX ctx;

	SHA1_Init( &ctx );
	if(v1) SHA1_Update( &ctx, v1, len1 );
	if(v2) SHA1_Update( &ctx, v2, len2 );
	if(v3) SHA1_Update( &ctx, v3, len3 );

	SHA1_Final( &ctx, hash_return );
}

int dht_random_bytes( void *buf, size_t size ) {
	return bytes_random( buf, size );
}

void kad_setup( void ) {
	UCHAR node_id[SHA1_BIN_LENGTH];
	int s4, s6;

	s4 = -1;
	s6 = -1;

	/* Let the DHT output debug text */
	if( gconf->verbosity == VERBOSITY_DEBUG ) {
		dht_debug = stdout;
	}

	bytes_from_hex( node_id, gconf->node_id_str, strlen( gconf->node_id_str ) );

	dht_lock_init();

	if( gconf->af == AF_INET ) {
		s4 = net_bind( "KAD", DHT_ADDR4, gconf->dht_port, gconf->dht_ifname, IPPROTO_UDP, AF_INET );
		net_add_handler( s4, &dht_handler );
	} else {
		s6 = net_bind( "KAD", DHT_ADDR6, gconf->dht_port, gconf->dht_ifname, IPPROTO_UDP, AF_INET6 );
		net_add_handler( s6, &dht_handler );
	}

	/* Init the DHT.  Also set the sockets into non-blocking mode. */
	if( dht_init( s4, s6, node_id, (UCHAR*) "KN\0\0") < 0 ) {
		log_err( "KAD: Failed to initialize the DHT." );
	}
}

void kad_free( void ) {
	dht_uninit();
}

int kad_count_nodes( int good ) {
	struct bucket *bucket;
	struct node *node;
	int count;

	bucket = (gconf->af == AF_INET ) ? buckets : buckets6;
	count = 0;
	while( bucket ) {
		if( good ) {
			node = bucket->nodes;
			while( node ) {
				count += node_good( node ) ? 1 : 0;
				node = node->next;
			}
		} else {
			count += bucket->count;
		}
		bucket = bucket->next;
	}
	return count;
}

#define bprintf(...) (written += snprintf( buf+written, size-written, __VA_ARGS__))

int kad_status( char *buf, int size ) {
	char hexbuf[SHA1_HEX_LENGTH+1];
	struct storage *strg = storage;
	struct search *srch = searches;
	int numsearches_active = 0;
	int numsearches_done = 0;
	int numstorage = 0;
	int numstorage_peers = 0;
	int numvalues = 0;
	int written = 0;

	/* count searches */
	while( srch != NULL ) {
		if( srch->done ) {
			numsearches_done++;
		} else {
			numsearches_active++;
		}
		srch = srch->next;
	}

	/* count storage and peers */
	while( strg != NULL ) {
		numstorage_peers += strg->numpeers;
		numstorage++;
		strg = strg->next;
	}

	numvalues = values_count();

	bprintf( "Version: %s\n", kadnode_version_str );
	bprintf( "DHT id: %s\n", str_id( myid, hexbuf ) );
	bprintf( "DHT bound to: %s:%s / %s\n",
		(gconf->af == AF_INET) ? "0.0.0.0" : "::",
		gconf->dht_port,
		(gconf->dht_ifname == NULL) ? "<any device>" : gconf->dht_ifname
   );

	bprintf( "DHT Nodes: %d (%d good) (%s)\n",
		kad_count_nodes( 0 ), kad_count_nodes( 1 ), (gconf->af == AF_INET) ? "IPv4" : "IPv6" );
	bprintf( "DHT Storage: %d (max %d), %d peers (max %d per storage)\n",
		numstorage, DHT_MAX_HASHES, numstorage_peers, DHT_MAX_PEERS );
	bprintf( "DHT Searches: %d active, %d completed (max %d)\n",
		numsearches_active, numsearches_done, DHT_MAX_SEARCHES );
	bprintf( "DHT Blacklist: %d (max %d)\n",
		(next_blacklisted % DHT_MAX_BLACKLISTED), DHT_MAX_BLACKLISTED );
	bprintf( "DHT Values to announce: %d\n", numvalues );

	return written;
}

int kad_ping( const IP* addr ) {
	int rc;

	dht_lock();
	rc = dht_ping_node( (struct sockaddr *)addr, addr_len( addr ) );
	dht_unlock();

	return (rc < 0) ? -1 : 0;
}

/*
* Find nodes that are near the given id and announce to them
* that this node can satisfy the given id on the given port.
*/
int kad_announce_once( const UCHAR id[], int port ) {

	if( port < 1 || port > 65535 ) {
		return -1;
	}

	dht_lock();
	dht_search( id, port, gconf->af, dht_callback_func, NULL );
	dht_unlock();

	return 0;
}

/*
* Add a new value to the announcement list or refresh an announcement.
*/
int kad_announce( const char _query[], int port, time_t lifetime ) {
	char query[QUERY_MAX_SIZE];

	/* Remove .p2p suffix and convert to lowercase */
	if( query_sanitize( query, sizeof(query), _query ) != 0 ) {
		return -1;
	}

	/* Store query to call kad_announce_once() later/multiple times */
	return values_add( query, port, lifetime , NULL, NULL) ? 0 : -2;
}

/*
* Lookup known nodes that are nearest to the given id.
*/
int kad_lookup_value( const char _query[], IP addr_array[], size_t *addr_num ,
        char *payload, char *date_str) {
	char query[QUERY_MAX_SIZE];
	struct results_t *results;
	int is_new;
	int rc;
    char buf[257];

	if( query_sanitize( query, sizeof(query), _query ) != 0 ) {
		return -2;
	}

	log_debug( "KAD: Lookup string: %s", query );

	dht_lock();

	/* Find existing or create new item */
	results = results_add( query, &is_new, payload, date_str);

	if( results && is_new ) {
		// Search own announced values 
		kad_lookup_local_values( results );
	}


	if( results == NULL ) {
		/* Failed to create a new search */
		rc = -1;	
    }

    /* Hajime
     * Kadnode doesn't always start a new search. For Hajime, always 
     * start new search
     */
     else if( results->done ) {
		/*
		* Restart finished search when no results have been found
		* or more than half of the search results lifetime has expired.
		*/
		//if( results_entries_count( results ) == 0 ||
		//	(time_now_sec() - results->start_time) > (MAX_SEARCH_LIFETIME / 2)
		//) {
			/* Restart search finished search */
			results_done( results, 0 );
			dht_search( results->id, 0, gconf->af, dht_callback_func, NULL );
		//}
		rc = 2;
	} 
    else if( is_new ) {
        values_debug_print("starting new search for %s\n", str_id(results->id, buf));
		/* A new search was started */
		dht_search( results->id, 0, gconf->af, dht_callback_func, NULL );
		rc = 1;
	} 
    else {
		/* Search is still running */
		rc = 0;
	}

    /*Hajime
     * no need to do this; we log results directly to file
     */
	//*addr_num = results_collect( results, addr_array, *addr_num );

	dht_unlock();

	return rc;
}

/*
* Lookup the address of the node that has the given id.
* The port refers to the kad instance.
*/
int kad_lookup_node( const char query[], IP *addr_return ) {
	UCHAR id[SHA1_BIN_LENGTH];
	struct search *sr;
	int i, rc;

	if( strlen( query ) != SHA1_HEX_LENGTH || !str_isHex( query, SHA1_HEX_LENGTH ) ) {
		return -1;
	}

	bytes_from_hex( id, query, SHA1_HEX_LENGTH );

	dht_lock();

	rc = 1;
	sr = searches;
	while( sr ) {
		if( sr->af == gconf->af && id_equal( sr->id, id ) ) {
			for( i = 0; i < sr->numnodes; ++i ) {
				if( id_equal( sr->nodes[i].id, id ) ) {
					memcpy( addr_return, &sr->nodes[i].ss, sizeof(IP) );
					rc = 0;
					goto done;
				}
			}
			break;
		}
		sr = sr->next;
	}

	done:;

	dht_unlock();

	return rc;
}

int kad_blacklist( const IP* addr ) {

	dht_lock();
	blacklist_node( NULL, (struct sockaddr *) addr, sizeof(IP) );
	dht_unlock();

	return 0;
}

/* Export known nodes; the maximum is 200 nodes */
int kad_export_nodes( IP addr_array[], size_t *num ) {
	IP4 *addr4;
	IP6 *addr6;
	int num4;
	int num6;
	int i;

	if( gconf->af == AF_INET6 ) {
		num6 = MIN(*num, 200);
		addr6 = calloc( num6, sizeof(IP6) );
		num4 = 0;
		addr4 = NULL;
	} else {
		num6 = 0;
		addr6 = NULL;
		num4 = MIN(*num, 200);
		addr4 = calloc( num4, sizeof(IP4) );
	}

	dht_lock();
	dht_get_nodes( addr4, &num4, addr6, &num6 );
	dht_unlock();

	if( gconf->af == AF_INET6 ) {
		for( i = 0; i < num6; ++i ) {
			memcpy( &addr_array[i], &addr6[i], sizeof(IP6) );
		}
		free( addr6 );
	} else {
		for( i = 0; i < num4; ++i ) {
			memcpy( &addr_array[i], &addr4[i], sizeof(IP4) );
		}
		free( addr4 );
	}

	/* Store number of nodes we have actually found */
	*num = i;

	return 0;
}

/* Print buckets (leaf/finger table) */
void kad_debug_buckets( int fd ) {
	char addrbuf[FULL_ADDSTRLEN+1];
	char hexbuf[SHA1_HEX_LENGTH+1];
	struct bucket *b;
	struct node *n;
	int i, j;

	dht_lock();

	b = (gconf->af == AF_INET) ? buckets : buckets6;
	for( j = 0; b != NULL; ++j ) {
		dprintf( fd, " Bucket: %s\n", str_id( b->first, hexbuf ) );

		n = b->nodes;
		for( i = 0; n != NULL; ++i ) {
			dprintf( fd, "   Node: %s\n", str_id( n->id, hexbuf ) );
			dprintf( fd, "    addr: %s\n", str_addr( &n->ss, addrbuf ) );
			dprintf( fd, "    pinged: %d\n", n->pinged );
			n = n->next;
		}
		dprintf( fd, "  Found %d nodes.\n", i );
		b = b->next;
	}
	dprintf( fd, " Found %d buckets.\n", j );

	dht_unlock();
}

/* Print searches */
void kad_debug_searches( int fd ) {
	char addrbuf[FULL_ADDSTRLEN+1];
	char hexbuf[SHA1_HEX_LENGTH+1];
	struct search *s = searches;
	int i, j;

	dht_lock();

	for( j = 0; s != NULL; ++j ) {
		dprintf( fd, " Search: %s\n", str_id( s->id, hexbuf ) );
		dprintf( fd, "  af: %s\n", (s->af == AF_INET) ? "AF_INET" : "AF_INET6" );
		dprintf( fd, "  port: %hu\n", s->port );
		dprintf( fd, "  done: %d\n", s->done );
		for(i = 0; i < s->numnodes; ++i) {
			struct search_node *sn = &s->nodes[i];
			dprintf( fd, "   Node: %s\n", str_id(sn->id, hexbuf ) );
			dprintf( fd, "    addr: %s\n", str_addr( &sn->ss, addrbuf ) );
			dprintf( fd, "    pinged: %d\n", sn->pinged );
			dprintf( fd, "    replied: %d\n", sn->replied );
			dprintf( fd, "    acked: %d\n", sn->acked );
		}
		dprintf( fd, "  Found %d nodes.\n", i );
		s = s->next;
	}
	dprintf( fd, " Found %d searches.\n", j );

	dht_unlock();
}

/* Print announced ids we have received */
void kad_debug_storage( int fd ) {
	char addrbuf[FULL_ADDSTRLEN+1];
	char hexbuf[SHA1_HEX_LENGTH+1];
	struct storage *s;
	struct peer* p;
	IP addr;
	int i, j;

	dht_lock();

	s = storage;
	for( j = 0; s != NULL; ++j ) {
		dprintf( fd, " ID: %s\n", str_id(s->id, hexbuf ));
		for( i = 0; i < s->numpeers; ++i ) {
			p = &s->peers[i];
			to_addr( &addr, &p->ip, p->len, htons( p->port ) );
			dprintf( fd, "   Peer: %s\n", str_addr( &addr, addrbuf)  );
		}
		dprintf( fd, "  Found %d peers.\n", i );
		s = s->next;
	}
	dprintf( fd, " Found %d stored hashes from received announcements.\n", j );

	dht_unlock();
}

void kad_debug_blacklist( int fd ) {
	char addrbuf[FULL_ADDSTRLEN+1];
	int i;

	dht_lock();

	for( i = 0; i < (next_blacklisted % DHT_MAX_BLACKLISTED); i++ ) {
		dprintf( fd, " %s\n", str_addr( &blacklist[i], addrbuf ) );
	}

	dprintf( fd, " Found %d blacklisted addresses.\n", i );

	dht_unlock();
}

void kad_debug_constants( int fd ) {
	dprintf( fd, "DHT_SEARCH_EXPIRE_TIME: %d\n", DHT_SEARCH_EXPIRE_TIME );
	dprintf( fd, "DHT_MAX_SEARCHES: %d\n", DHT_MAX_SEARCHES );

	/* maximum number of announced hashes we track */
	dprintf( fd, "DHT_MAX_HASHES: %d\n", DHT_MAX_HASHES );

	/* maximum number of peers for each announced hash we track */
	dprintf( fd, "DHT_MAX_PEERS: %d\n", DHT_MAX_PEERS );

	/* maximum number of blacklisted nodes */
	dprintf( fd, "DHT_MAX_BLACKLISTED: %d\n", DHT_MAX_BLACKLISTED );
}

/* Hajime
 * Log info to a file. Start a new logfile each day, and append
 * the date to the filename. What is logged is determined by 
 * DEBUG macros defined in kad.h.
 */
void log_print(const char *str, ...){
    char time_str[26];

    // Get the current time in utc for timestamp
    time_t now;
    struct tm *utc_time;
    time(&now);
    utc_time = gmtime(&now);
    strftime(time_str, 26, "%F", utc_time); 
    
    //open logfile for result infohash
    char filename[257];
    errno = 0;
    snprintf(filename, sizeof(filename), "lookup_log_%s.log", time_str);
    FILE *log = fopen(filename, "a");

    fprintf(log, "%ld: ", time_now_sec());

    va_list arglist;
    va_start(arglist,str);
    vfprintf(log,str,arglist);
    va_end(arglist);
    fclose(log);
}
