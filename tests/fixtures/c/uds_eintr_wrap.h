#ifndef NETIPC_UDS_EINTR_WRAP_H
#define NETIPC_UDS_EINTR_WRAP_H

/* Make the next send_count send(), sendmsg_count sendmsg() and recv_count
 * recv() calls fail with EINTR. Linked only where --wrap is available. */
void uds_eintr_wrap_set(unsigned send_count, unsigned sendmsg_count,
                        unsigned recv_count);

/* Injections not consumed yet. */
unsigned uds_eintr_wrap_pending(void);

/* Inside the next bind(), create the unrelated file `path` with mode 0666,
 * as another thread of the process could, and record its resulting mode. */
void uds_bind_probe_arm(const char *path);

/* Mode bits of the file the armed probe created, or -1 if no bind ran. */
int uds_bind_probe_mode(void);

#endif
