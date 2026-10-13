# Assignment 5 skeleton

Read this together with the assignment document (LG_Assignment5_Final_Scheduling.docx, Korean edition LG_Assignment5_Final_Scheduling_KO.docx). The programs in src/ are complete: frameloop, svc, and svcload build and run as they are. What you write are the three bpftrace tools in bt/. Until their TODOs are filled in, the scripts run but leave their maps empty.

```
make                                   # bin/frameloop, bin/svc, bin/svcload
sudo ./bin/frameloop -g none -n 60     # Level 1 program (SCHED_DEADLINE)
bash tools/l2_run.sh none              # Level 2. Starts svc, reqtrace.bt and svcload in that order
bash tools/l3_run.sh case 1            # Level 3. Starts mystery and attaches whydelay.bt (binary only)
```

| File | What you do |
| --- | --- |
| bt/frametrace.bt | TODO(L1-5) on-CPU time and runnable off-CPU time from sched_switch, (L1-6) SIGXCPU generation |
| bt/reqtrace.bt | TODO(L2-5) queue / service / reply segments, @svc_wait, @preempted, @prio_at_begin, @reported_prio (rx is given as an example) |
| bt/whydelay.bt | TODO(L3-1) the whole script (see the design and hints in the comments) |
| src/frameloop.c, src/svc.c | Provided. Read them: the explanations ask you to cite their KEY lines. Do not modify |
| src/svcload.c, src/svc_proto.h, src/a5.h | Provided. Do not modify |

- Run bpftrace from inside this folder. The uprobes in the scripts find the programs through `./bin/...` paths.
- The uprobe anchor functions are frame_begin, frame_end, req_enqueue, handle_begin, handle_end, req_send, and req_done.
- l2_run.sh and l3_run.sh ask for the sudo password once and then run as root. Type one command at a time.
