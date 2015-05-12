/*
 * Copyright (C) 2004-2012 Kay Sievers <kay@vrfy.org>
 * Copyright (C) 2004 Chris Friesen <chris_friesen@sympatico.ca>
 * Copyright (C) 2009 Canonical Ltd.
 * Copyright (C) 2009 Scott James Remnant <scott@netsplit.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stddef.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <getopt.h>
#include <sys/file.h>
#include <sys/time.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/inotify.h>

#include "sd-daemon.h"
#include "rtnl-util.h"
#include "cgroup-util.h"
#include "dev-setup.h"
#include "fileio.h"
#include "selinux-util.h"
#include "udev.h"
#include "udev-util.h"
#include "formats-util.h"
#include "hashmap.h"

static struct udev_rules *rules;
static struct udev_ctrl *udev_ctrl;
static struct udev_ctrl_connection *udev_ctrl_conn;
static struct udev_monitor *monitor;
static int worker_watch[2] = { -1, -1 };
static int fd_signal = -1;
static int fd_ep = -1;
static int fd_inotify = -1;
static bool stop_exec_queue;
static bool reload;
static bool arg_debug = false;
static int arg_daemonize = false;
static int arg_resolve_names = 1;
static unsigned arg_children_max;
static int arg_exec_delay;
static usec_t arg_event_timeout_usec = 180 * USEC_PER_SEC;
static usec_t arg_event_timeout_warn_usec = 180 * USEC_PER_SEC / 3;
static sigset_t sigmask_orig;
static UDEV_LIST(event_list);
Hashmap *workers;
static char *udev_cgroup;
static struct udev_list properties_list;
static bool udev_exit;

enum event_state {
        EVENT_UNDEF,
        EVENT_QUEUED,
        EVENT_RUNNING,
};

struct event {
        struct udev_list_node node;
        struct udev *udev;
        struct udev_device *dev;
        struct udev_device *dev_kernel;
        struct worker *worker;
        enum event_state state;
        unsigned long long int delaying_seqnum;
        unsigned long long int seqnum;
        const char *devpath;
        size_t devpath_len;
        const char *devpath_old;
        dev_t devnum;
        int ifindex;
        bool is_block;
        usec_t start_usec;
        bool warned;
};

static inline struct event *node_to_event(struct udev_list_node *node) {
        return container_of(node, struct event, node);
}

static void event_queue_cleanup(struct udev *udev, enum event_state type);

enum worker_state {
        WORKER_UNDEF,
        WORKER_RUNNING,
        WORKER_IDLE,
        WORKER_KILLED,
};

struct worker {
        struct udev_list_node node;
        struct udev *udev;
        int refcount;
        pid_t pid;
        struct udev_monitor *monitor;
        enum worker_state state;
        struct event *event;
};

/* passed from worker to main process */
struct worker_message {
};

static void event_free(struct event *event) {
        if (!event)
                return;

        udev_list_node_remove(&event->node);
        udev_device_unref(event->dev);
        udev_device_unref(event->dev_kernel);

        if (event->worker)
                event->worker->event = NULL;

        free(event);
}

static void worker_free(struct worker *worker) {
        if (!worker)
                return;

        hashmap_remove(workers, UINT_TO_PTR(worker->pid));
        udev_monitor_unref(worker->monitor);
        udev_unref(worker->udev);
        event_free(worker->event);

        free(worker);
}

static void workers_free(void) {
        struct worker *worker;
        Iterator i;

        HASHMAP_FOREACH(worker, workers, i)
                worker_free(worker);

        hashmap_free(workers);
        workers = NULL;
}

static int worker_new(struct worker **ret, struct udev *udev, struct udev_monitor *worker_monitor, pid_t pid) {
        _cleanup_free_ struct worker *worker = NULL;
        int r;

        assert(ret);
        assert(udev);
        assert(worker_monitor);
        assert(pid > 1);

        worker = new0(struct worker, 1);
        if (!worker)
                return -ENOMEM;

        worker->refcount = 1;
        worker->udev = udev_ref(udev);
        /* close monitor, but keep address around */
        udev_monitor_disconnect(worker_monitor);
        worker->monitor = udev_monitor_ref(worker_monitor);
        worker->pid = pid;

        r = hashmap_ensure_allocated(&workers, NULL);
        if (r < 0)
                return r;

        r = hashmap_put(workers, UINT_TO_PTR(pid), worker);
        if (r < 0)
                return r;

        *ret = worker;
        worker = NULL;

        return 0;
}

static void worker_attach_event(struct worker *worker, struct event *event) {
        assert(worker);
        assert(event);
        assert(!event->worker);
        assert(!worker->event);

        worker->state = WORKER_RUNNING;
        worker->event = event;
        event->state = EVENT_RUNNING;
        event->start_usec = now(CLOCK_MONOTONIC);
        event->warned = false;
        event->worker = worker;
}

static void worker_spawn(struct event *event) {
        struct udev *udev = event->udev;
        _cleanup_udev_monitor_unref_ struct udev_monitor *worker_monitor = NULL;
        pid_t pid;

        /* listen for new events */
        worker_monitor = udev_monitor_new_from_netlink(udev, NULL);
        if (worker_monitor == NULL)
                return;
        /* allow the main daemon netlink address to send devices to the worker */
        udev_monitor_allow_unicast_sender(worker_monitor, monitor);
        udev_monitor_enable_receiving(worker_monitor);

        pid = fork();
        switch (pid) {
        case 0: {
                struct udev_device *dev = NULL;
                int fd_monitor;
                _cleanup_rtnl_unref_ sd_rtnl *rtnl = NULL;
                struct epoll_event ep_signal, ep_monitor;
                sigset_t mask;
                int r = 0;

                /* take initial device from queue */
                dev = event->dev;
                event->dev = NULL;

                workers_free();
                event_queue_cleanup(udev, EVENT_UNDEF);
                udev_monitor_unref(monitor);
                udev_ctrl_unref(udev_ctrl);
                close(fd_signal);
                close(fd_ep);
                close(worker_watch[READ_END]);

                sigfillset(&mask);
                fd_signal = signalfd(-1, &mask, SFD_NONBLOCK|SFD_CLOEXEC);
                if (fd_signal < 0) {
                        r = log_error_errno(errno, "error creating signalfd %m");
                        goto out;
                }

                fd_ep = epoll_create1(EPOLL_CLOEXEC);
                if (fd_ep < 0) {
                        r = log_error_errno(errno, "error creating epoll fd: %m");
                        goto out;
                }

                memzero(&ep_signal, sizeof(struct epoll_event));
                ep_signal.events = EPOLLIN;
                ep_signal.data.fd = fd_signal;

                fd_monitor = udev_monitor_get_fd(worker_monitor);
                memzero(&ep_monitor, sizeof(struct epoll_event));
                ep_monitor.events = EPOLLIN;
                ep_monitor.data.fd = fd_monitor;

                if (epoll_ctl(fd_ep, EPOLL_CTL_ADD, fd_signal, &ep_signal) < 0 ||
                    epoll_ctl(fd_ep, EPOLL_CTL_ADD, fd_monitor, &ep_monitor) < 0) {
                        r = log_error_errno(errno, "fail to add fds to epoll: %m");
                        goto out;
                }

                /* request TERM signal if parent exits */
                prctl(PR_SET_PDEATHSIG, SIGTERM);

                /* reset OOM score, we only protect the main daemon */
                write_string_file("/proc/self/oom_score_adj", "0");

                for (;;) {
                        struct udev_event *udev_event;
                        struct worker_message msg;
                        int fd_lock = -1;

                        log_debug("seq %llu running", udev_device_get_seqnum(dev));
                        udev_event = udev_event_new(dev);
                        if (udev_event == NULL) {
                                r = -ENOMEM;
                                goto out;
                        }

                        /* needed for SIGCHLD/SIGTERM in spawn() */
                        udev_event->fd_signal = fd_signal;

                        if (arg_exec_delay > 0)
                                udev_event->exec_delay = arg_exec_delay;

                        /*
                         * Take a shared lock on the device node; this establishes
                         * a concept of device "ownership" to serialize device
                         * access. External processes holding an exclusive lock will
                         * cause udev to skip the event handling; in the case udev
                         * acquired the lock, the external process can block until
                         * udev has finished its event handling.
                         */
                        if (!streq_ptr(udev_device_get_action(dev), "remove") &&
                            streq_ptr("block", udev_device_get_subsystem(dev)) &&
                            !startswith(udev_device_get_sysname(dev), "dm-") &&
                            !startswith(udev_device_get_sysname(dev), "md")) {
                                struct udev_device *d = dev;

                                if (streq_ptr("partition", udev_device_get_devtype(d)))
                                        d = udev_device_get_parent(d);

                                if (d) {
                                        fd_lock = open(udev_device_get_devnode(d), O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
                                        if (fd_lock >= 0 && flock(fd_lock, LOCK_SH|LOCK_NB) < 0) {
                                                log_debug_errno(errno, "Unable to flock(%s), skipping event handling: %m", udev_device_get_devnode(d));
                                                fd_lock = safe_close(fd_lock);
                                                r = -EAGAIN;
                                                goto skip;
                                        }
                                }
                        }

                        /* needed for renaming netifs */
                        udev_event->rtnl = rtnl;

                        /* apply rules, create node, symlinks */
                        udev_event_execute_rules(udev_event,
                                                 arg_event_timeout_usec, arg_event_timeout_warn_usec,
                                                 &properties_list,
                                                 rules,
                                                 &sigmask_orig);

                        udev_event_execute_run(udev_event,
                                               arg_event_timeout_usec, arg_event_timeout_warn_usec,
                                               &sigmask_orig);

                        if (udev_event->rtnl)
                                /* in case rtnl was initialized */
                                rtnl = sd_rtnl_ref(udev_event->rtnl);

                        /* apply/restore inotify watch */
                        if (udev_event->inotify_watch) {
                                udev_watch_begin(udev, dev);
                                udev_device_update_db(dev);
                        }

                        safe_close(fd_lock);

                        /* send processed event back to libudev listeners */
                        udev_monitor_send_device(worker_monitor, NULL, dev);

skip:
                        log_debug("seq %llu processed", udev_device_get_seqnum(dev));

                        /* send udevd the result of the event execution */
                        memzero(&msg, sizeof(struct worker_message));
                        r = send(worker_watch[WRITE_END], &msg, sizeof(struct worker_message), 0);
                        if (r < 0)
                                log_error_errno(errno, "failed to send result of seq %llu to main daemon: %m",
                                                udev_device_get_seqnum(dev));

                        udev_device_unref(dev);
                        dev = NULL;

                        if (udev_event->sigterm) {
                                udev_event_unref(udev_event);
                                goto out;
                        }

                        udev_event_unref(udev_event);

                        /* wait for more device messages from main udevd, or term signal */
                        while (dev == NULL) {
                                struct epoll_event ev[4];
                                int fdcount;
                                int i;

                                fdcount = epoll_wait(fd_ep, ev, ELEMENTSOF(ev), -1);
                                if (fdcount < 0) {
                                        if (errno == EINTR)
                                                continue;
                                        r = log_error_errno(errno, "failed to poll: %m");
                                        goto out;
                                }

                                for (i = 0; i < fdcount; i++) {
                                        if (ev[i].data.fd == fd_monitor && ev[i].events & EPOLLIN) {
                                                dev = udev_monitor_receive_device(worker_monitor);
                                                break;
                                        } else if (ev[i].data.fd == fd_signal && ev[i].events & EPOLLIN) {
                                                struct signalfd_siginfo fdsi;
                                                ssize_t size;

                                                size = read(fd_signal, &fdsi, sizeof(struct signalfd_siginfo));
                                                if (size != sizeof(struct signalfd_siginfo))
                                                        continue;
                                                switch (fdsi.ssi_signo) {
                                                case SIGTERM:
                                                        goto out;
                                                }
                                        }
                                }
                        }
                }
out:
                udev_device_unref(dev);
                safe_close(fd_signal);
                safe_close(fd_ep);
                close(fd_inotify);
                close(worker_watch[WRITE_END]);
                udev_rules_unref(rules);
                udev_builtin_exit(udev);
                udev_unref(udev);
                log_close();
                _exit(r < 0 ? EXIT_FAILURE : EXIT_SUCCESS);
        }
        case -1:
                event->state = EVENT_QUEUED;
                log_error_errno(errno, "fork of child failed: %m");
                break;
        default:
        {
                struct worker *worker;
                int r;

                r = worker_new(&worker, udev, worker_monitor, pid);
                if (r < 0)
                        return;

                worker_attach_event(worker, event);

                log_debug("seq %llu forked new worker ["PID_FMT"]", udev_device_get_seqnum(event->dev), pid);
                break;
        }
        }
}

static void event_run(struct event *event) {
        struct worker *worker;
        Iterator i;

        HASHMAP_FOREACH(worker, workers, i) {
                ssize_t count;

                if (worker->state != WORKER_IDLE)
                        continue;

                count = udev_monitor_send_device(monitor, worker->monitor, event->dev);
                if (count < 0) {
                        log_error_errno(errno, "worker ["PID_FMT"] did not accept message %zi (%m), kill it",
                                        worker->pid, count);
                        kill(worker->pid, SIGKILL);
                        worker->state = WORKER_KILLED;
                        continue;
                }
                worker_attach_event(worker, event);
                return;
        }

        if (hashmap_size(workers) >= arg_children_max) {
                if (arg_children_max > 1)
                        log_debug("maximum number (%i) of children reached", hashmap_size(workers));
                return;
        }

        /* start new worker and pass initial device */
        worker_spawn(event);
}

static int event_queue_insert(struct udev_device *dev) {
        struct event *event;

        event = new0(struct event, 1);
        if (event == NULL)
                return -1;

        event->udev = udev_device_get_udev(dev);
        event->dev = dev;
        event->dev_kernel = udev_device_shallow_clone(dev);
        udev_device_copy_properties(event->dev_kernel, dev);
        event->seqnum = udev_device_get_seqnum(dev);
        event->devpath = udev_device_get_devpath(dev);
        event->devpath_len = strlen(event->devpath);
        event->devpath_old = udev_device_get_devpath_old(dev);
        event->devnum = udev_device_get_devnum(dev);
        event->is_block = streq("block", udev_device_get_subsystem(dev));
        event->ifindex = udev_device_get_ifindex(dev);

        log_debug("seq %llu queued, '%s' '%s'", udev_device_get_seqnum(dev),
             udev_device_get_action(dev), udev_device_get_subsystem(dev));

        event->state = EVENT_QUEUED;
        udev_list_node_append(&event->node, &event_list);
        return 0;
}

static void worker_kill(void) {
        struct worker *worker;
        Iterator i;

        HASHMAP_FOREACH(worker, workers, i) {
                if (worker->state == WORKER_KILLED)
                        continue;

                worker->state = WORKER_KILLED;
                kill(worker->pid, SIGTERM);
        }
}

/* lookup event for identical, parent, child device */
static bool is_devpath_busy(struct event *event) {
        struct udev_list_node *loop;
        size_t common;

        /* check if queue contains events we depend on */
        udev_list_node_foreach(loop, &event_list) {
                struct event *loop_event = node_to_event(loop);

                /* we already found a later event, earlier can not block us, no need to check again */
                if (loop_event->seqnum < event->delaying_seqnum)
                        continue;

                /* event we checked earlier still exists, no need to check again */
                if (loop_event->seqnum == event->delaying_seqnum)
                        return true;

                /* found ourself, no later event can block us */
                if (loop_event->seqnum >= event->seqnum)
                        break;

                /* check major/minor */
                if (major(event->devnum) != 0 && event->devnum == loop_event->devnum && event->is_block == loop_event->is_block)
                        return true;

                /* check network device ifindex */
                if (event->ifindex != 0 && event->ifindex == loop_event->ifindex)
                        return true;

                /* check our old name */
                if (event->devpath_old != NULL && streq(loop_event->devpath, event->devpath_old)) {
                        event->delaying_seqnum = loop_event->seqnum;
                        return true;
                }

                /* compare devpath */
                common = MIN(loop_event->devpath_len, event->devpath_len);

                /* one devpath is contained in the other? */
                if (memcmp(loop_event->devpath, event->devpath, common) != 0)
                        continue;

                /* identical device event found */
                if (loop_event->devpath_len == event->devpath_len) {
                        /* devices names might have changed/swapped in the meantime */
                        if (major(event->devnum) != 0 && (event->devnum != loop_event->devnum || event->is_block != loop_event->is_block))
                                continue;
                        if (event->ifindex != 0 && event->ifindex != loop_event->ifindex)
                                continue;
                        event->delaying_seqnum = loop_event->seqnum;
                        return true;
                }

                /* parent device event found */
                if (event->devpath[common] == '/') {
                        event->delaying_seqnum = loop_event->seqnum;
                        return true;
                }

                /* child device event found */
                if (loop_event->devpath[common] == '/') {
                        event->delaying_seqnum = loop_event->seqnum;
                        return true;
                }

                /* no matching device */
                continue;
        }

        return false;
}

static void event_queue_start(struct udev *udev) {
        struct udev_list_node *loop;

        udev_list_node_foreach(loop, &event_list) {
                struct event *event = node_to_event(loop);

                if (event->state != EVENT_QUEUED)
                        continue;

                /* do not start event if parent or child event is still running */
                if (is_devpath_busy(event))
                        continue;

                event_run(event);
        }
}

static void event_queue_cleanup(struct udev *udev, enum event_state match_type) {
        struct udev_list_node *loop, *tmp;

        udev_list_node_foreach_safe(loop, tmp, &event_list) {
                struct event *event = node_to_event(loop);

                if (match_type != EVENT_UNDEF && match_type != event->state)
                        continue;

                event_free(event);
        }
}

static int on_worker(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        for (;;) {
                struct worker_message msg;
                struct iovec iovec = {
                        .iov_base = &msg,
                        .iov_len = sizeof(msg),
                };
                union {
                        struct cmsghdr cmsghdr;
                        uint8_t buf[CMSG_SPACE(sizeof(struct ucred))];
                } control = {};
                struct msghdr msghdr = {
                        .msg_iov = &iovec,
                        .msg_iovlen = 1,
                        .msg_control = &control,
                        .msg_controllen = sizeof(control),
                };
                struct cmsghdr *cmsg;
                ssize_t size;
                struct ucred *ucred = NULL;
                struct worker *worker;

                size = recvmsg(fd, &msghdr, MSG_DONTWAIT);
                if (size < 0) {
                        if (errno == EAGAIN || errno == EINTR)
                                return 1;

                        return log_error_errno(errno, "failed to receive message: %m");
                } else if (size != sizeof(struct worker_message)) {
                        log_warning_errno(EIO, "ignoring worker message with invalid size %zi bytes", size);
                        continue;
                }

                for (cmsg = CMSG_FIRSTHDR(&msghdr); cmsg; cmsg = CMSG_NXTHDR(&msghdr, cmsg)) {
                        if (cmsg->cmsg_level == SOL_SOCKET &&
                            cmsg->cmsg_type == SCM_CREDENTIALS &&
                            cmsg->cmsg_len == CMSG_LEN(sizeof(struct ucred)))
                                ucred = (struct ucred*) CMSG_DATA(cmsg);
                }

                if (!ucred || ucred->pid <= 0) {
                        log_warning_errno(EIO, "ignoring worker message without valid PID");
                        continue;
                }

                /* lookup worker who sent the signal */
                worker = hashmap_get(workers, UINT_TO_PTR(ucred->pid));
                if (!worker) {
                        log_debug("worker ["PID_FMT"] returned, but is no longer tracked", ucred->pid);
                        continue;
                }

                if (worker->state != WORKER_KILLED)
                        worker->state = WORKER_IDLE;

                /* worker returned */
                event_free(worker->event);
        }

        return 1;
}

static int on_uevent(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        struct udev_monitor *m = userdata;
        struct udev_device *dev;
        int r;

        assert(m);

        dev = udev_monitor_receive_device(m);
        if (dev) {
                udev_device_ensure_usec_initialized(dev, NULL);
                r = event_queue_insert(dev);
                if (r < 0)
                        udev_device_unref(dev);
        }

        return 1;
}

/* receive the udevd message from userspace */
static int on_ctrl_msg(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        struct udev_ctrl *uctrl = userdata;
        _cleanup_udev_ctrl_connection_unref_ struct udev_ctrl_connection *ctrl_conn = NULL;
        _cleanup_udev_ctrl_msg_unref_ struct udev_ctrl_msg *ctrl_msg = NULL;
        const char *str;
        int i;

        assert(uctrl);

        ctrl_conn = udev_ctrl_get_connection(uctrl);
        if (!ctrl_conn)
                return 1;

        ctrl_msg = udev_ctrl_receive_msg(ctrl_conn);
        if (!ctrl_msg)
                return 1;

        i = udev_ctrl_get_set_log_level(ctrl_msg);
        if (i >= 0) {
                log_debug("udevd message (SET_LOG_LEVEL) received, log_priority=%i", i);
                log_set_max_level(i);
                worker_kill();
        }

        if (udev_ctrl_get_stop_exec_queue(ctrl_msg) > 0) {
                log_debug("udevd message (STOP_EXEC_QUEUE) received");
                stop_exec_queue = true;
        }

        if (udev_ctrl_get_start_exec_queue(ctrl_msg) > 0) {
                log_debug("udevd message (START_EXEC_QUEUE) received");
                stop_exec_queue = false;
        }

        if (udev_ctrl_get_reload(ctrl_msg) > 0) {
                log_debug("udevd message (RELOAD) received");
                reload = true;
        }

        str = udev_ctrl_get_set_env(ctrl_msg);
        if (str != NULL) {
                char *key;

                key = strdup(str);
                if (key != NULL) {
                        char *val;

                        val = strchr(key, '=');
                        if (val != NULL) {
                                val[0] = '\0';
                                val = &val[1];
                                if (val[0] == '\0') {
                                        log_debug("udevd message (ENV) received, unset '%s'", key);
                                        udev_list_entry_add(&properties_list, key, NULL);
                                } else {
                                        log_debug("udevd message (ENV) received, set '%s=%s'", key, val);
                                        udev_list_entry_add(&properties_list, key, val);
                                }
                        } else {
                                log_error("wrong key format '%s'", key);
                        }
                        free(key);
                }
                worker_kill();
        }

        i = udev_ctrl_get_set_children_max(ctrl_msg);
        if (i >= 0) {
                log_debug("udevd message (SET_MAX_CHILDREN) received, children_max=%i", i);
                arg_children_max = i;
        }

        if (udev_ctrl_get_ping(ctrl_msg) > 0)
                log_debug("udevd message (SYNC) received");

        if (udev_ctrl_get_exit(ctrl_msg) > 0) {
                log_debug("udevd message (EXIT) received");
                udev_exit = true;
                /* keep reference to block the client until we exit */
                udev_ctrl_conn = udev_ctrl_connection_ref(ctrl_conn);
        }

        return 1;
}

static int synthesize_change(struct udev_device *dev) {
        char filename[UTIL_PATH_SIZE];
        int r;

        if (streq_ptr("block", udev_device_get_subsystem(dev)) &&
            streq_ptr("disk", udev_device_get_devtype(dev)) &&
            !startswith(udev_device_get_sysname(dev), "dm-")) {
                bool part_table_read = false;
                bool has_partitions = false;
                int fd;
                struct udev *udev = udev_device_get_udev(dev);
                _cleanup_udev_enumerate_unref_ struct udev_enumerate *e = NULL;
                struct udev_list_entry *item;

                /*
                 * Try to re-read the partition table. This only succeeds if
                 * none of the devices is busy. The kernel returns 0 if no
                 * partition table is found, and we will not get an event for
                 * the disk.
                 */
                fd = open(udev_device_get_devnode(dev), O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
                if (fd >= 0) {
                        r = flock(fd, LOCK_EX|LOCK_NB);
                        if (r >= 0)
                                r = ioctl(fd, BLKRRPART, 0);

                        close(fd);
                        if (r >= 0)
                                part_table_read = true;
                }

                /* search for partitions */
                e = udev_enumerate_new(udev);
                if (!e)
                        return -ENOMEM;

                r = udev_enumerate_add_match_parent(e, dev);
                if (r < 0)
                        return r;

                r = udev_enumerate_add_match_subsystem(e, "block");
                if (r < 0)
                        return r;

                r = udev_enumerate_scan_devices(e);
                if (r < 0)
                        return r;

                udev_list_entry_foreach(item, udev_enumerate_get_list_entry(e)) {
                        _cleanup_udev_device_unref_ struct udev_device *d = NULL;

                        d = udev_device_new_from_syspath(udev, udev_list_entry_get_name(item));
                        if (!d)
                                continue;

                        if (!streq_ptr("partition", udev_device_get_devtype(d)))
                                continue;

                        has_partitions = true;
                        break;
                }

                /*
                 * We have partitions and re-read the table, the kernel already sent
                 * out a "change" event for the disk, and "remove/add" for all
                 * partitions.
                 */
                if (part_table_read && has_partitions)
                        return 0;

                /*
                 * We have partitions but re-reading the partition table did not
                 * work, synthesize "change" for the disk and all partitions.
                 */
                log_debug("device %s closed, synthesising 'change'", udev_device_get_devnode(dev));
                strscpyl(filename, sizeof(filename), udev_device_get_syspath(dev), "/uevent", NULL);
                write_string_file(filename, "change");

                udev_list_entry_foreach(item, udev_enumerate_get_list_entry(e)) {
                        _cleanup_udev_device_unref_ struct udev_device *d = NULL;

                        d = udev_device_new_from_syspath(udev, udev_list_entry_get_name(item));
                        if (!d)
                                continue;

                        if (!streq_ptr("partition", udev_device_get_devtype(d)))
                                continue;

                        log_debug("device %s closed, synthesising partition '%s' 'change'",
                                  udev_device_get_devnode(dev), udev_device_get_devnode(d));
                        strscpyl(filename, sizeof(filename), udev_device_get_syspath(d), "/uevent", NULL);
                        write_string_file(filename, "change");
                }

                return 0;
        }

        log_debug("device %s closed, synthesising 'change'", udev_device_get_devnode(dev));
        strscpyl(filename, sizeof(filename), udev_device_get_syspath(dev), "/uevent", NULL);
        write_string_file(filename, "change");

        return 0;
}

static int on_inotify(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        struct udev *udev = userdata;
        union inotify_event_buffer buffer;
        struct inotify_event *e;
        ssize_t l;

        assert(udev);

        l = read(fd, &buffer, sizeof(buffer));
        if (l < 0) {
                if (errno == EAGAIN || errno == EINTR)
                        return 1;

                return log_error_errno(errno, "Failed to read inotify fd: %m");
        }

        FOREACH_INOTIFY_EVENT(e, buffer, l) {
                _cleanup_udev_device_unref_ struct udev_device *dev = NULL;

                dev = udev_watch_lookup(udev, e->wd);
                if (!dev)
                        continue;

                log_debug("inotify event: %x for %s", e->mask, udev_device_get_devnode(dev));
                if (e->mask & IN_CLOSE_WRITE) {
                        synthesize_change(dev);

                        /* settle might be waiting on us to determine the queue
                         * state. If we just handled an inotify event, we might have
                         * generated a "change" event, but we won't have queued up
                         * the resultant uevent yet. Do that.
                         */
                        on_uevent(NULL, -1, 0, monitor);
                } else if (e->mask & IN_IGNORED)
                        udev_watch_end(udev, dev);
        }

        return 1;
}

static int on_request_exit(sd_event_source *s, const struct signalfd_siginfo *si, void *userdata) {
        udev_exit = true;

        return 1;
}

static int on_request_reload(sd_event_source *s, const struct signalfd_siginfo *si, void *userdata) {
        reload = true;

        return 1;
}

static int on_sigchld(sd_event_source *s, const struct signalfd_siginfo *si, void *userdata) {
        for (;;) {
                pid_t pid;
                int status;
                struct worker *worker;

                pid = waitpid(-1, &status, WNOHANG);
                if (pid <= 0)
                        return 1;

                worker = hashmap_get(workers, UINT_TO_PTR(pid));
                if (!worker) {
                        log_warning("worker ["PID_FMT"] is unknown, ignoring", pid);
                        return 1;
                }

                if (WIFEXITED(status)) {
                        if (WEXITSTATUS(status) == 0)
                                log_debug("worker ["PID_FMT"] exited", pid);
                        else
                                log_warning("worker ["PID_FMT"] exited with return code %i", pid, WEXITSTATUS(status));
                } else if (WIFSIGNALED(status)) {
                        log_warning("worker ["PID_FMT"] terminated by signal %i (%s)", pid, WTERMSIG(status), strsignal(WTERMSIG(status)));
                } else if (WIFSTOPPED(status)) {
                        log_info("worker ["PID_FMT"] stopped", pid);
                        return 1;
                } else if (WIFCONTINUED(status)) {
                        log_info("worker ["PID_FMT"] continued", pid);
                        return 1;
                } else
                        log_warning("worker ["PID_FMT"] exit with status 0x%04x", pid, status);

                if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                        if (worker->event) {
                                log_error("worker ["PID_FMT"] failed while handling '%s'", pid, worker->event->devpath);
                                /* delete state from disk */
                                udev_device_delete_db(worker->event->dev);
                                udev_device_tag_index(worker->event->dev, NULL, false);
                                /* forward kernel event without amending it */
                                udev_monitor_send_device(monitor, NULL, worker->event->dev_kernel);
                        }
                }

                worker_free(worker);
        }

        return 1;
}

static void event_queue_update(void) {
        int r;

        if (!udev_list_node_is_empty(&event_list)) {
                r = touch("/run/udev/queue");
                if (r < 0)
                        log_warning_errno(r, "could not touch /run/udev/queue: %m");
        } else {
                r = unlink("/run/udev/queue");
                if (r < 0 && errno != ENOENT)
                        log_warning("could not unlink /run/udev/queue: %m");
        }
}

static int systemd_fds(struct udev *udev, int *rctrl, int *rnetlink) {
        int ctrl = -1, netlink = -1;
        int fd, n;

        n = sd_listen_fds(true);
        if (n <= 0)
                return -1;

        for (fd = SD_LISTEN_FDS_START; fd < n + SD_LISTEN_FDS_START; fd++) {
                if (sd_is_socket(fd, AF_LOCAL, SOCK_SEQPACKET, -1)) {
                        if (ctrl >= 0)
                                return -1;
                        ctrl = fd;
                        continue;
                }

                if (sd_is_socket(fd, AF_NETLINK, SOCK_RAW, -1)) {
                        if (netlink >= 0)
                                return -1;
                        netlink = fd;
                        continue;
                }

                return -1;
        }

        if (ctrl < 0 || netlink < 0)
                return -1;

        log_debug("ctrl=%i netlink=%i", ctrl, netlink);
        *rctrl = ctrl;
        *rnetlink = netlink;
        return 0;
}

/*
 * read the kernel command line, in case we need to get into debug mode
 *   udev.log-priority=<level>                 syslog priority
 *   udev.children-max=<number of workers>     events are fully serialized if set to 1
 *   udev.exec-delay=<number of seconds>       delay execution of every executed program
 *   udev.event-timeout=<number of seconds>    seconds to wait before terminating an event
 */
static int parse_proc_cmdline_item(const char *key, const char *value) {
        int r;

        assert(key);

        if (!value)
                return 0;

        if (startswith(key, "rd."))
                key += strlen("rd.");

        if (startswith(key, "udev."))
                key += strlen("udev.");
        else
                return 0;

        if (streq(key, "log-priority")) {
                int prio;

                prio = util_log_priority(value);
                log_set_max_level(prio);
        } else if (streq(key, "children-max")) {
                r = safe_atou(value, &arg_children_max);
                if (r < 0)
                        log_warning("invalid udev.children-max ignored: %s", value);
        } else if (streq(key, "exec-delay")) {
                r = safe_atoi(value, &arg_exec_delay);
                if (r < 0)
                        log_warning("invalid udev.exec-delay ignored: %s", value);
        } else if (streq(key, "event-timeout")) {
                r = safe_atou64(value, &arg_event_timeout_usec);
                if (r < 0)
                        log_warning("invalid udev.event-timeout ignored: %s", value);
                else {
                        arg_event_timeout_usec *= USEC_PER_SEC;
                        arg_event_timeout_warn_usec = (arg_event_timeout_usec / 3) ? : 1;
                }
        }

        return 0;
}

static void help(void) {
        printf("%s [OPTIONS...]\n\n"
               "Manages devices.\n\n"
               "  -h --help                   Print this message\n"
               "     --version                Print version of the program\n"
               "     --daemon                 Detach and run in the background\n"
               "     --debug                  Enable debug output\n"
               "     --children-max=INT       Set maximum number of workers\n"
               "     --exec-delay=SECONDS     Seconds to wait before executing RUN=\n"
               "     --event-timeout=SECONDS  Seconds to wait before terminating an event\n"
               "     --resolve-names=early|late|never\n"
               "                              When to resolve users and groups\n"
               , program_invocation_short_name);
}

static int parse_argv(int argc, char *argv[]) {
        static const struct option options[] = {
                { "daemon",             no_argument,            NULL, 'd' },
                { "debug",              no_argument,            NULL, 'D' },
                { "children-max",       required_argument,      NULL, 'c' },
                { "exec-delay",         required_argument,      NULL, 'e' },
                { "event-timeout",      required_argument,      NULL, 't' },
                { "resolve-names",      required_argument,      NULL, 'N' },
                { "help",               no_argument,            NULL, 'h' },
                { "version",            no_argument,            NULL, 'V' },
                {}
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "c:de:DtN:hV", options, NULL)) >= 0) {
                int r;

                switch (c) {

                case 'd':
                        arg_daemonize = true;
                        break;
                case 'c':
                        r = safe_atou(optarg, &arg_children_max);
                        if (r < 0)
                                log_warning("Invalid --children-max ignored: %s", optarg);
                        break;
                case 'e':
                        r = safe_atoi(optarg, &arg_exec_delay);
                        if (r < 0)
                                log_warning("Invalid --exec-delay ignored: %s", optarg);
                        break;
                case 't':
                        r = safe_atou64(optarg, &arg_event_timeout_usec);
                        if (r < 0)
                                log_warning("Invalid --event-timeout ignored: %s", optarg);
                        else {
                                arg_event_timeout_usec *= USEC_PER_SEC;
                                arg_event_timeout_warn_usec = (arg_event_timeout_usec / 3) ? : 1;
                        }
                        break;
                case 'D':
                        arg_debug = true;
                        break;
                case 'N':
                        if (streq(optarg, "early")) {
                                arg_resolve_names = 1;
                        } else if (streq(optarg, "late")) {
                                arg_resolve_names = 0;
                        } else if (streq(optarg, "never")) {
                                arg_resolve_names = -1;
                        } else {
                                log_error("resolve-names must be early, late or never");
                                return 0;
                        }
                        break;
                case 'h':
                        help();
                        return 0;
                case 'V':
                        printf("%s\n", VERSION);
                        return 0;
                case '?':
                        return -EINVAL;
                default:
                        assert_not_reached("Unhandled option");

                }
        }

        return 1;
}

int main(int argc, char *argv[]) {
        struct udev *udev;
        sigset_t mask;
        int fd_ctrl = -1;
        int fd_netlink = -1;
        int fd_worker = -1;
        struct epoll_event ep_ctrl = { .events = EPOLLIN };
        struct epoll_event ep_inotify = { .events = EPOLLIN };
        struct epoll_event ep_signal = { .events = EPOLLIN };
        struct epoll_event ep_netlink = { .events = EPOLLIN };
        struct epoll_event ep_worker = { .events = EPOLLIN };
        int r = 0, one = 1;

        udev = udev_new();
        if (!udev) {
                r = log_error_errno(errno, "could not allocate udev context: %m");
                goto exit;
        }

        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        r = parse_argv(argc, argv);
        if (r <= 0)
                goto exit;

        r = parse_proc_cmdline(parse_proc_cmdline_item);
        if (r < 0)
                log_warning_errno(r, "failed to parse kernel command line, ignoring: %m");

        if (arg_debug)
                log_set_max_level(LOG_DEBUG);

        if (getuid() != 0) {
                r = log_error_errno(EPERM, "root privileges required");
                goto exit;
        }

        r = mac_selinux_init("/dev");
        if (r < 0) {
                log_error_errno(r, "could not initialize labelling: %m");
                goto exit;
        }

        /* set umask before creating any file/directory */
        r = chdir("/");
        if (r < 0) {
                r = log_error_errno(errno, "could not change dir to /: %m");
                goto exit;
        }

        umask(022);

        udev_list_init(udev, &properties_list, true);

        r = mkdir("/run/udev", 0755);
        if (r < 0 && errno != EEXIST) {
                r = log_error_errno(errno, "could not create /run/udev: %m");
                goto exit;
        }

        dev_setup(NULL);

        /* before opening new files, make sure std{in,out,err} fds are in a sane state */
        if (arg_daemonize) {
                int fd;

                fd = open("/dev/null", O_RDWR);
                if (fd >= 0) {
                        if (write(STDOUT_FILENO, 0, 0) < 0)
                                dup2(fd, STDOUT_FILENO);
                        if (write(STDERR_FILENO, 0, 0) < 0)
                                dup2(fd, STDERR_FILENO);
                        if (fd > STDERR_FILENO)
                                close(fd);
                } else {
                        log_error("cannot open /dev/null");
                }
        }

        if (systemd_fds(udev, &fd_ctrl, &fd_netlink) >= 0) {
                /* get control and netlink socket from systemd */
                udev_ctrl = udev_ctrl_new_from_fd(udev, fd_ctrl);
                if (!udev_ctrl) {
                        r = log_error_errno(EINVAL, "error taking over udev control socket");
                        goto exit;
                }

                monitor = udev_monitor_new_from_netlink_fd(udev, "kernel", fd_netlink);
                if (!monitor) {
                        r = log_error_errno(EINVAL, "error taking over netlink socket");
                        goto exit;
                }

                /* get our own cgroup, we regularly kill everything udev has left behind */
                if (cg_pid_get_path(SYSTEMD_CGROUP_CONTROLLER, 0, &udev_cgroup) < 0)
                        udev_cgroup = NULL;
        } else {
                /* open control and netlink socket */
                udev_ctrl = udev_ctrl_new(udev);
                if (!udev_ctrl) {
                        r = log_error_errno(EINVAL, "error initializing udev control socket");
                        goto exit;
                }
                fd_ctrl = udev_ctrl_get_fd(udev_ctrl);

                monitor = udev_monitor_new_from_netlink(udev, "kernel");
                if (!monitor) {
                        r = log_error_errno(EINVAL, "error initializing netlink socket");
                        goto exit;
                }
                fd_netlink = udev_monitor_get_fd(monitor);

                udev_monitor_set_receive_buffer_size(monitor, 128 * 1024 * 1024);
        }

        if (udev_monitor_enable_receiving(monitor) < 0) {
                r = log_error_errno(EINVAL, "error binding netlink socket");
                goto exit;
        }

        if (udev_ctrl_enable_receiving(udev_ctrl) < 0) {
                r = log_error_errno(EINVAL, "error binding udev control socket");
                goto exit;
        }

        log_info("starting version " VERSION);

        udev_builtin_init(udev);

        rules = udev_rules_new(udev, arg_resolve_names);
        if (!rules) {
                r = log_error_errno(ENOMEM, "error reading rules");
                goto exit;
        }

        r = udev_rules_apply_static_dev_perms(rules);
        if (r < 0)
                log_error_errno(r, "failed to apply permissions on static device nodes: %m");

        if (arg_daemonize) {
                pid_t pid;

                pid = fork();
                switch (pid) {
                case 0:
                        break;
                case -1:
                        r = log_error_errno(errno, "fork of daemon failed: %m");
                        goto exit;
                default:
                        goto exit_daemonize;
                }

                setsid();

                write_string_file("/proc/self/oom_score_adj", "-1000");
        } else {
                sd_notify(1, "READY=1");
        }

        if (arg_children_max == 0) {
                cpu_set_t cpu_set;

                arg_children_max = 8;

                if (sched_getaffinity(0, sizeof (cpu_set), &cpu_set) == 0) {
                        arg_children_max +=  CPU_COUNT(&cpu_set) * 2;
                }
        }
        log_debug("set children_max to %u", arg_children_max);

        udev_list_node_init(&event_list);

        fd_inotify = udev_watch_init(udev);
        if (fd_inotify < 0) {
                r = log_error_errno(ENOMEM, "error initializing inotify");
                goto exit;
        }
        udev_watch_restore(udev);

        /* block and listen to all signals on signalfd */
        sigfillset(&mask);
        sigprocmask(SIG_SETMASK, &mask, &sigmask_orig);
        fd_signal = signalfd(-1, &mask, SFD_NONBLOCK|SFD_CLOEXEC);
        if (fd_signal < 0) {
                r = log_error_errno(errno, "error creating signalfd");
                goto exit;
        }

        /* unnamed socket from workers to the main daemon */
        if (socketpair(AF_LOCAL, SOCK_DGRAM|SOCK_CLOEXEC, 0, worker_watch) < 0) {
                r = log_error_errno(errno, "error creating socketpair");
                goto exit;
        }
        fd_worker = worker_watch[READ_END];

        r = setsockopt(fd_worker, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one));
        if (r < 0)
                return log_error_errno(errno, "could not enable SO_PASSCRED: %m");

        ep_ctrl.data.fd = fd_ctrl;
        ep_inotify.data.fd = fd_inotify;
        ep_signal.data.fd = fd_signal;
        ep_netlink.data.fd = fd_netlink;
        ep_worker.data.fd = fd_worker;

        fd_ep = epoll_create1(EPOLL_CLOEXEC);
        if (fd_ep < 0) {
                log_error_errno(errno, "error creating epoll fd: %m");
                goto exit;
        }
        if (epoll_ctl(fd_ep, EPOLL_CTL_ADD, fd_ctrl, &ep_ctrl) < 0 ||
            epoll_ctl(fd_ep, EPOLL_CTL_ADD, fd_inotify, &ep_inotify) < 0 ||
            epoll_ctl(fd_ep, EPOLL_CTL_ADD, fd_signal, &ep_signal) < 0 ||
            epoll_ctl(fd_ep, EPOLL_CTL_ADD, fd_netlink, &ep_netlink) < 0 ||
            epoll_ctl(fd_ep, EPOLL_CTL_ADD, fd_worker, &ep_worker) < 0) {
                log_error_errno(errno, "fail to add fds to epoll: %m");
                goto exit;
        }

        for (;;) {
                static usec_t last_usec;
                struct epoll_event ev[8];
                int fdcount;
                int timeout;
                bool is_worker, is_signal, is_inotify, is_netlink, is_ctrl;
                int i;

                if (udev_exit) {
                        /* close sources of new events and discard buffered events */
                        if (fd_ctrl >= 0) {
                                epoll_ctl(fd_ep, EPOLL_CTL_DEL, fd_ctrl, NULL);
                                fd_ctrl = -1;
                        }
                        if (monitor != NULL) {
                                epoll_ctl(fd_ep, EPOLL_CTL_DEL, fd_netlink, NULL);
                                udev_monitor_unref(monitor);
                                monitor = NULL;
                        }
                        if (fd_inotify >= 0) {
                                epoll_ctl(fd_ep, EPOLL_CTL_DEL, fd_inotify, NULL);
                                close(fd_inotify);
                                fd_inotify = -1;
                        }

                        /* discard queued events and kill workers */
                        event_queue_cleanup(udev, EVENT_QUEUED);
                        worker_kill();

                        /* exit after all has cleaned up */
                        if (udev_list_node_is_empty(&event_list) && hashmap_isempty(workers))
                                break;

                        /* timeout at exit for workers to finish */
                        timeout = 30 * MSEC_PER_SEC;
                } else if (udev_list_node_is_empty(&event_list) && hashmap_isempty(workers)) {
                        /* we are idle */
                        timeout = -1;

                        /* cleanup possible left-over processes in our cgroup */
                        if (udev_cgroup)
                                cg_kill(SYSTEMD_CGROUP_CONTROLLER, udev_cgroup, SIGKILL, false, true, NULL);
                } else {
                        /* kill idle or hanging workers */
                        timeout = 3 * MSEC_PER_SEC;
                }

                /* tell settle that we are busy or idle */
                event_queue_update();

                fdcount = epoll_wait(fd_ep, ev, ELEMENTSOF(ev), timeout);
                if (fdcount < 0)
                        continue;

                if (fdcount == 0) {
                        struct worker *worker;
                        Iterator j;

                        /* timeout */
                        if (udev_exit) {
                                log_error("timeout, giving up waiting for workers to finish");
                                break;
                        }

                        /* kill idle workers */
                        if (udev_list_node_is_empty(&event_list)) {
                                log_debug("cleanup idle workers");
                                worker_kill();
                        }

                        /* check for hanging events */
                        HASHMAP_FOREACH(worker, workers, j) {
                                struct event *event = worker->event;
                                usec_t ts;

                                if (worker->state != WORKER_RUNNING)
                                        continue;

                                assert(event);

                                ts = now(CLOCK_MONOTONIC);

                                if ((ts - event->start_usec) > arg_event_timeout_warn_usec) {
                                        if ((ts - event->start_usec) > arg_event_timeout_usec) {
                                                log_error("worker ["PID_FMT"] %s timeout; kill it", worker->pid, event->devpath);
                                                kill(worker->pid, SIGKILL);
                                                worker->state = WORKER_KILLED;

                                                log_error("seq %llu '%s' killed", udev_device_get_seqnum(event->dev), event->devpath);
                                        } else if (!event->warned) {
                                                log_warning("worker ["PID_FMT"] %s is taking a long time", worker->pid, event->devpath);
                                                event->warned = true;
                                        }
                                }
                        }

                }

                is_worker = is_signal = is_inotify = is_netlink = is_ctrl = false;
                for (i = 0; i < fdcount; i++) {
                        if (ev[i].data.fd == fd_worker && ev[i].events & EPOLLIN)
                                is_worker = true;
                        else if (ev[i].data.fd == fd_netlink && ev[i].events & EPOLLIN)
                                is_netlink = true;
                        else if (ev[i].data.fd == fd_signal && ev[i].events & EPOLLIN)
                                is_signal = true;
                        else if (ev[i].data.fd == fd_inotify && ev[i].events & EPOLLIN)
                                is_inotify = true;
                        else if (ev[i].data.fd == fd_ctrl && ev[i].events & EPOLLIN)
                                is_ctrl = true;
                }

                /* check for changed config, every 3 seconds at most */
                if ((now(CLOCK_MONOTONIC) - last_usec) > 3 * USEC_PER_SEC) {
                        if (udev_rules_check_timestamp(rules))
                                reload = true;
                        if (udev_builtin_validate(udev))
                                reload = true;

                        last_usec = now(CLOCK_MONOTONIC);
                }

                /* reload requested, HUP signal received, rules changed, builtin changed */
                if (reload) {
                        worker_kill();
                        rules = udev_rules_unref(rules);
                        udev_builtin_exit(udev);
                        reload = false;
                }

                /* event has finished */
                if (is_worker)
                        on_worker(NULL, fd_worker, 0, NULL);

                /* uevent from kernel */
                if (is_netlink)
                        on_uevent(NULL, fd_netlink, 0, monitor);

                /* start new events */
                if (!udev_list_node_is_empty(&event_list) && !udev_exit && !stop_exec_queue) {
                        udev_builtin_init(udev);
                        if (rules == NULL)
                                rules = udev_rules_new(udev, arg_resolve_names);
                        if (rules != NULL)
                                event_queue_start(udev);
                }

                if (is_signal) {
                        struct signalfd_siginfo fdsi;
                        ssize_t size;

                        size = read(fd_signal, &fdsi, sizeof(struct signalfd_siginfo));
                        if (size == sizeof(struct signalfd_siginfo)) {
                                switch (fdsi.ssi_signo) {
                                case SIGINT:
                                case SIGTERM:
                                        on_request_exit(NULL, &fdsi, NULL);
                                        break;
                                case SIGHUP:
                                        on_request_reload(NULL, &fdsi, NULL);
                                        break;
                                case SIGCHLD:
                                        on_sigchld(NULL, &fdsi, NULL);
                                        break;
                                }
                        }
                }

                /* we are shutting down, the events below are not handled anymore */
                if (udev_exit)
                        continue;

                /* device node watch */
                if (is_inotify)
                        on_inotify(NULL, fd_inotify, 0, udev);

                /* tell settle that we are busy or idle, this needs to be before the
                 * PING handling
                 */
                event_queue_update();

                /*
                 * This needs to be after the inotify handling, to make sure,
                 * that the ping is send back after the possibly generated
                 * "change" events by the inotify device node watch.
                 *
                 * A single time we may receive a client connection which we need to
                 * keep open to block the client. It will be closed right before we
                 * exit.
                 */
                if (is_ctrl)
                        on_ctrl_msg(NULL, fd_ctrl, 0, udev_ctrl);
        }

exit:
        udev_ctrl_cleanup(udev_ctrl);
        unlink("/run/udev/queue");
exit_daemonize:
        if (fd_ep >= 0)
                close(fd_ep);
        workers_free();
        event_queue_cleanup(udev, EVENT_UNDEF);
        udev_rules_unref(rules);
        udev_builtin_exit(udev);
        if (fd_signal >= 0)
                close(fd_signal);
        if (worker_watch[READ_END] >= 0)
                close(worker_watch[READ_END]);
        if (worker_watch[WRITE_END] >= 0)
                close(worker_watch[WRITE_END]);
        udev_monitor_unref(monitor);
        udev_ctrl_connection_unref(udev_ctrl_conn);
        udev_ctrl_unref(udev_ctrl);
        udev_list_cleanup(&properties_list);
        mac_selinux_finish();
        udev_unref(udev);
        log_close();
        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
