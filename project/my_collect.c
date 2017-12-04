#include <stdbool.h>
#include "contiki.h"
#include "lib/random.h"
#include "net/rime/rime.h"
#include "leds.h"
#include "net/netstack.h"
#include <stdio.h>
#include "core/net/linkaddr.h"
#include "my_collect.h"

#define NUMBER_NODES 5
#define BEACON_INTERVAL (CLOCK_SECOND*60)
#define REPORT_INTERVAL (CLOCK_SECOND*60*2)
#define BEACON_FORWARD_DELAY (random_rand() % CLOCK_SECOND)
#define REPORT_FORWARD_DELAY (random_rand() % CLOCK_SECOND)
#define RSSI_THRESHOLD -95


// Many-to-one
void bc_recv(struct broadcast_conn *conn, const linkaddr_t *sender);
void uc_recv(struct unicast_conn *c, const linkaddr_t *from);
void beacon_timer_cb(void* ptr);
/* Callback structures */
struct broadcast_callbacks bc_cb = {.recv=bc_recv};
struct unicast_callbacks uc_cb = {.recv=uc_recv};

// One-to-many
void sr_send(struct unicast_conn *c, const linkaddr_t *dest);
void sr_recv(struct unicast_conn *c, const linkaddr_t *from);

// Report
void rpt_recv(struct unicast_conn *c, const linkaddr_t *sender);
void report_timer_cb(void* ptr); 
//Callback structures
struct unicast_callbacks rpt_cb = {.recv=rpt_recv};


/*--------------------------------------------------------------------------------------*/

void my_collect_open(struct my_collect_conn* conn, uint16_t channels, 
                     bool is_sink, const struct my_collect_callbacks *callbacks)
{
  int i;
  linkaddr_t temp;
  temp.u8[0] = 0x00;
  temp.u8[0] = 0x00;
  // initialise the connector structure
  linkaddr_copy(&conn->parent, &linkaddr_null);
  conn->metric = 65535; // the max metric (means that the node is not connected yet)
  conn->beacon_seqn = 0;
  conn->callbacks = callbacks;

  // open the underlying primitives
  broadcast_open(&conn->bc, channels,     &bc_cb);
  unicast_open  (&conn->uc, channels + 1, &uc_cb);
  unicast_open  (&conn->rpt, channels + 2, &rpt_cb);

  // TODO 1: make the sink send beacons periodically
  if(is_sink)
  {
    conn->metric = 0;
    linkaddr_copy(&conn->parent, &linkaddr_node_addr); // Set the sink as father of himself 
    // Init the routing table
    conn->routing_table = malloc(NUMBER_NODES * sizeof(linkaddr_t*));
    for (i = 0; i < NUMBER_NODES; i++) 
    {
      conn->routing_table[i] = malloc(2 * sizeof(int));
      // Set every entry to zero
      conn->routing_table[i][0] = temp;
      conn->routing_table[i][1] = temp;
    }
    // Set the beacon timer
    ctimer_set(&conn->beacon_timer, BEACON_INTERVAL, beacon_timer_cb, conn);
    // Send immediatly a beacon for not way the first interval
    send_beacon(conn);
  }
}

/* Handling beacons --------------------------------------------------------------------*/

struct beacon_msg { // Beacon message structure
  uint16_t seqn;
  uint16_t metric;
} __attribute__((packed));

// Send beacon using the current seqn and metric
void send_beacon(struct my_collect_conn* conn) {
  struct beacon_msg beacon = {.seqn = conn->beacon_seqn, .metric = conn->metric};

  packetbuf_clear();
  packetbuf_copyfrom(&beacon, sizeof(beacon));
  printf("my_collect-beacon: sending beacon: seqn %d metric %d\n", conn->beacon_seqn, conn->metric);
  broadcast_send(&conn->bc);
}

// Beacon timer callback
void beacon_timer_cb(void* ptr) {
  // TODO 2: implement the beacon callback
  struct my_collect_conn* conn = (struct my_collect_conn*) ptr;

  send_beacon(conn);
  
  ctimer_set(&conn->beacon_timer, BEACON_INTERVAL, beacon_timer_cb, conn);
}

// Beacon receive callback
void bc_recv(struct broadcast_conn *bc_conn, const linkaddr_t *sender) {
  struct beacon_msg beacon;
  int8_t rssi;
  bool update = false;
  // Get the pointer to the overall structure my_collect_conn from its field bc
  struct my_collect_conn* conn = (struct my_collect_conn*)(((uint8_t*)bc_conn) - 
    offsetof(struct my_collect_conn, bc));

  if (packetbuf_datalen() != sizeof(struct beacon_msg)) {
    printf("my_collect: broadcast of wrong size\n");
    return;
  }
  memcpy(&beacon, packetbuf_dataptr(), sizeof(struct beacon_msg));
  rssi = packetbuf_attr(PACKETBUF_ATTR_RSSI);
  printf("my_collect: recv beacon from %02x:%02x seqn %u metric %u rssi %d\n", 
      sender->u8[0], sender->u8[1], 
      beacon.seqn, beacon.metric, rssi);

  // TODO 3: analyse the received beacon, update the routing info (parent, metric), if needed
  printf("my_collect: conn metric %u, beacon metric %u\n", conn->metric, beacon.metric);
  // Check if the metric of the packet is better than the existing one
  if(beacon.metric < conn->metric)
  {
    // Check that the RSSI is quite good to use the channel
    if(rssi > -95)
    {
      printf("my_collect: Set conn metric %u to beacon metric %u\n", conn->metric, ++beacon.metric);
      conn->metric = beacon.metric++;
      printf("my_collect: Set conn parent %02x:%02x to sender %02x:%02x\n", conn->parent.u8[0], conn->parent.u8[0], sender->u8[0], sender->u8[1]);
      conn->parent = *sender;
      update = true;
    }
  }

  // TODO 4: retransmit the beacon if the metric has been updated
  if(update)
  {
    update = false;
    // Only the sink increments the seqn
    // conn->beacon_seqn = conn->beacon_seqn++;

    // Send beacon
    ctimer_set(&conn->beacon_timer, BEACON_FORWARD_DELAY, beacon_timer_cb, conn);
    // send_beacon(conn);

    // Send report
    send_report(conn);
  }
}


/* Handling topology reports ----------------------------------------------------------*/

struct report_msg { // Report message structure
  uint16_t seqn;
  linkaddr_t source;
  linkaddr_t parent;
} __attribute__((packed));

// Send report using the current seqn, source and parent 
void send_report(struct my_collect_conn* conn) {
  struct report_msg rpt = {.seqn = conn->report_seqn, .source=linkaddr_node_addr, .parent=conn->parent};
  
  if (linkaddr_cmp(&conn->parent, &linkaddr_null))
    return 0; // no parent

  printf("my_collect-report: sending unicast: %02x:%02x\n", conn->parent.u8[0], conn->parent.u8[0]);
  packetbuf_clear();
  packetbuf_copyfrom(&rpt, sizeof(rpt));
  unicast_send(&conn->rpt, &conn->parent);
}

// Report timer callback
void report_timer_cb(void* ptr) {
  struct my_collect_conn* conn = (struct my_collect_conn*) ptr;

  send_report(conn);
  
  ctimer_set(&conn->report_timer, REPORT_INTERVAL, report_timer_cb, conn);
}

// Report receive callback
void rpt_recv(struct unicast_conn *uc_conn, const linkaddr_t *sender) {
  int i;
  struct report_msg rpt;

  // Get the pointer to the overall structure my_collect_conn from its field bc
  struct my_collect_conn* conn = (struct my_collect_conn*)(((uint8_t*)uc_conn) - 
    offsetof(struct my_collect_conn, rpt));

  if (packetbuf_datalen() != sizeof(struct report_msg)) {
    printf("my_collect: unicast of wrong size\n");
    return;
  }
  memcpy(&rpt, packetbuf_dataptr(), sizeof(struct report_msg));
  printf("my_collect: recv report from %02x:%02x\n", 
      sender->u8[0], sender->u8[1]);
 
  // Send report
  if(linkaddr_cmp(&conn->parent, &linkaddr_node_addr))
  {
    printf("rep_recv: sink source: %02x:%02x parent %02x:%02x\n", rpt.source.u8[0], rpt.source.u8[1], 
      rpt.parent.u8[0], rpt.parent.u8[1]);
    printf("rep_recv: sink routing table:\n");
    // Add/update entry in the routing table
    for(i = 0; i < NUMBER_NODES; i++)
    {
      if(linkaddr_cmp(&conn->routing_table[i][0], &rpt.source))
      {
        linkaddr_copy(&conn->routing_table[i][1], &rpt.parent);
        break;
      }
      else
      {
        if(conn->routing_table[i][0].u8[0] == 0x00 && conn->routing_table[i][0].u8[1] == 0x00)
        {
          linkaddr_copy(&conn->routing_table[i][0], &rpt.source);
          linkaddr_copy(&conn->routing_table[i][1], &rpt.parent);
          break;
        } 
      }

      printf("routing table: %02x:%02x - %02x:%02x\n", conn->routing_table[i][0].u8[0], conn->routing_table[i][0].u8[1], 
        conn->routing_table[i][1].u8[0], conn->routing_table[i][1].u8[1]);
    }

    printf("routing table: %02x:%02x - %02x:%02x\n", conn->routing_table[i][0].u8[0], conn->routing_table[i][0].u8[1], 
        conn->routing_table[i][1].u8[0], conn->routing_table[i][1].u8[1]);
  }
  else
  {
    printf("rep_recv: sending unicast: %02x:%02x\n", conn->parent.u8[0], conn->parent.u8[0]);
    packetbuf_clear();
    packetbuf_copyfrom(&rpt, sizeof(rpt));
    unicast_send(&conn->rpt, &conn->parent);
  }
}


/* Handling packets forwarding --------------------------------------------------------*/

void sr_send(struct unicast_conn *c, const linkaddr_t *dest)
{

}

void sr_recv(struct unicast_conn *c, const linkaddr_t *from)
{

}


/* Handling data packets --------------------------------------------------------------*/

struct collect_header { // Header structure for data packets
  linkaddr_t source;
  uint8_t hops;
} __attribute__((packed));

// Our send function
int my_collect_send(struct my_collect_conn *conn) {
  struct collect_header hdr = {.source=linkaddr_node_addr, .hops=0};
  int ret; 
  struct collect_header *hdrptr;

  if (linkaddr_cmp(&conn->parent, &linkaddr_null))
    return 0; // no parent

  // TODO 5:
  //  - allocate space for the header
  //  - insert the header
  //  - send the packet to the parent using unicast
  printf("my_collect: sending unicast: %02x:%02x\n", conn->parent.u8[0], conn->parent.u8[0]);
  packetbuf_hdralloc(sizeof(hdr));
  hdrptr = packetbuf_hdrptr();
  memcpy(hdrptr, &hdr, sizeof(hdr));
  unicast_send(&conn->uc, &conn->parent);
}

// Data receive callback
void uc_recv(struct unicast_conn *uc_conn, const linkaddr_t *from) {
  // Get the pointer to the overall structure my_collect_conn from its field uc
  struct my_collect_conn* conn = (struct my_collect_conn*)(((uint8_t*)uc_conn) - 
    offsetof(struct my_collect_conn, uc));

  struct collect_header hdr;
  struct collect_header *hdrptr;

  if (packetbuf_datalen() < sizeof(struct collect_header)) {
    printf("my_collect: too short unicast packet %d\n", packetbuf_datalen());
    return;
  }

  // TODO 6:
  //  - extract the header
  //  - on the sink, remove the header and call the application callback
  //  - on a router, update the header and forward the packet to the parent using unicast
  memcpy(&hdr, packetbuf_dataptr(), sizeof(hdr));
  packetbuf_hdrreduce(sizeof(hdr));
  hdr.hops = hdr.hops++;

  printf("uc_recv: from %02x:%02x, header: source %02x:%02x hops %u\n", 
    from->u8[0], from->u8[1], hdr.source.u8[0], hdr.source.u8[1], hdr.hops);

  if(linkaddr_cmp(&conn->parent, &linkaddr_node_addr))
  {
    printf("uc_recv: sink\n");
    conn->callbacks->recv(&hdr.source, hdr.hops);    
  }
  else
  {
    printf("uc_recv: sending unicast: %02x:%02x\n", conn->parent.u8[0], conn->parent.u8[0]);
    packetbuf_hdralloc(sizeof(hdr));
    hdrptr = packetbuf_hdrptr();
    memcpy(hdrptr, &hdr, sizeof(hdr));
    unicast_send(&conn->uc, &conn->parent);
  }
}
