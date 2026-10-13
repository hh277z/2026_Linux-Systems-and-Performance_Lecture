/* svc_proto.h: request/reply format between svcload (clients) and svc (service). Do not change. */
#ifndef SVC_PROTO_H
#define SVC_PROTO_H
#include <stdint.h>

#define SVC_ADDR "\0lg-a5-svc" /* abstract unix socket name: no file on disk */
#define CL_INPUT 1             /* request id = (client << 32) | sequence */
#define CL_LOGGER 2

struct svc_req {
    uint64_t id;
    int32_t client;
    int32_t prio;      /* rt priority of the sending thread (0 = not rt) */
    uint32_t work_us;  /* CPU time the service spends on this request */
    uint32_t pad;
    uint64_t t_send;   /* CLOCK_MONOTONIC ns at send */
};

struct svc_rep {
    uint64_t id;
    int32_t served_prio; /* rt priority the worker had while serving */
    int32_t queued;      /* requests waiting in the queue when this one started */
    uint64_t t_begin, t_end;
};
#endif
