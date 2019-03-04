
#ifndef _KAD_H_
#define _KAD_H_

/* File with Hajime payload data. Expects each line to be of the form
 *      <infohash> <payload_name> <date_used_to_construct_infohash>
 * This is generated by running update_infohashes.sh
 */
#define HASH_FILENAME "/home/ubuntu/hajime_dht/config/infohashes.txt"

/* Directory where the results from lookups will be written to
 */
#define LOOKUP_DATA_DIR "/home/ubuntu/hajime_dht/data/lookup"

/* Directory where data about individual nodes sending us results will
 * be written to
 */
#define RESULT_NODE_DATA_DIR "/home/ubuntu/hajime_dht/data/result_nodes"

#define PORTS_PER_ANNOUNCE 10

#define LOOKUPS

#define VALUES_DEBUG 1
#define SEARCH_DEBUG 1
#define AP_DEBUG 0
#define RESULTS_DEBUG 0
#define LOOKUP_DEBUG 0

void log_print(const char *str, ...);

// print debugging info about get_peer value results received
#define values_debug_print(...) \
    do { if (VALUES_DEBUG) log_print(__VA_ARGS__); } while (0)

// print debugging info about search structs and when they complete
#define search_debug_print(...) \
    do { if (SEARCH_DEBUG) log_print(__VA_ARGS__); } while (0)

// print debugging info about sending announce_peer messages
#define ap_debug_print(...) \
    do { if (AP_DEBUG) log_print(__VA_ARGS__); } while (0)

// print debugging info about results struct
#define results_debug_print(...) \
    do { if (RESULTS_DEBUG) log_print(__VA_ARGS__); } while (0)

// print debugging info about sending lookups
#define lookup_debug_print(...) \
    do { if (LOOKUP_DEBUG) log_print(__VA_ARGS__); } while (0)

/*
* Interface to interact with the DHT implementation.
* Negative return values indicate an error, success otherwise.
*/


void kad_setup( void );
void kad_free( void );

/* Ping this node to add it to the node table */
int kad_ping( const IP *addr );

/* Blacklist a specific address */
int kad_blacklist( const IP* addr );

/*
* Lookup the address of the node whose node id matches id.
* The lookup will be performed on the results of kad_lookup_value().
*/
int kad_lookup_node( const char query[], IP *addr_return );

/*
* Lookup the addresses of the nodes who have announced value id.
* The first call will start the search.
*/
int kad_lookup_value( const char query[], IP addr_array[], size_t *addr_num ,
        char *payload, char *date_str);

/* Export good nodes */
int kad_export_nodes( IP addr_array[], size_t *addr_num );

/* Print status information */
int kad_status( char *buf, int len );

/* Count good or all known peers */
int kad_count_nodes( int good );

/*
* Announce that the resource identified by id can
* be served by this computer using the given port.
*/
int kad_announce_once( const UCHAR id[], int port );

/* Announce query until lifetime expires. */
int kad_announce( const char query[], int port, time_t lifetime );

/* Various debug functions */
void kad_debug_buckets( int fd );
void kad_debug_searches( int fd );
void kad_debug_storage( int fd );
void kad_debug_blacklist( int fd );
void kad_debug_constants( int fd );

#endif /* _KAD_H_ */