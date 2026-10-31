#ifndef NETIPC_UDS_EINTR_WRAP_H
#define NETIPC_UDS_EINTR_WRAP_H

/* Make the next send_count send(), sendmsg_count sendmsg() and recv_count
 * recv() calls fail with EINTR. Linked only where --wrap is available. */
void uds_eintr_wrap_set(unsigned send_count, unsigned sendmsg_count,
                        unsigned recv_count);

/* Injections not consumed yet. */
unsigned uds_eintr_wrap_pending(void);

#endif
