#ifndef MY_COLLECT_H
#define MY_COLLECT_H

#include <stdbool.h>
#include "contiki.h"
#include "net/rime/rime.h"
#include "net/netstack.h"
#include "core/net/linkaddr.h"

/* Callback structure */
struct my_collect_callbacks {
  void (* recv)(const linkaddr_t *originator, uint8_t hops);
};

/* Connection object */
struct my_collect_conn {
  struct broadcast_conn bc;
  struct unicast_conn uc;
  struct unicast_conn rpt;
  const struct my_collect_callbacks* callbacks;
  linkaddr_t parent;
  linkaddr_t** routing_table;
  struct ctimer beacon_timer;
  struct ctimer report_timer;
  uint16_t metric;
  uint16_t beacon_seqn;
  uint16_t report_seqn;
};


/* Initialize a collect connection 
 *  - conn -- a pointer to a connection object 
 *  - channels -- starting channel C (the collect uses two: C and C+1)
 *  - is_sink -- initialize in either sink or router mode
 *  - callbacks -- a pointer to the callback structure */
void my_collect_open(
    struct my_collect_conn* conn, 
    uint16_t channels, 
    bool is_sink,
    const struct my_collect_callbacks *callbacks);

/* Send packet to the sink */
int  my_collect_send(struct my_collect_conn *c);

#endif //MY_COLLECT_H
