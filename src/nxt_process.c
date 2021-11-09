
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_main_process.h>

#if (NXT_HAVE_CLONE)
#include <nxt_clone.h>
#endif

#include <signal.h>

#if (NXT_HAVE_PR_SET_NO_NEW_PRIVS)
#include <sys/prctl.h>
#endif

static nxt_pid_t nxt_process_create(nxt_task_t *task, nxt_process_t *process);
static nxt_int_t nxt_process_setup(nxt_task_t *task, nxt_process_t *process);
static nxt_int_t nxt_process_child_fixup(nxt_task_t *task,
    nxt_process_t *process);
static nxt_int_t nxt_process_send_created(nxt_task_t *task,
    nxt_process_t *process);
static nxt_int_t nxt_process_send_ready(nxt_task_t *task,
    nxt_process_t *process);
static void nxt_process_created_ok(nxt_task_t *task, nxt_port_recv_msg_t *msg,
    void *data);
static void nxt_process_created_error(nxt_task_t *task,
    nxt_port_recv_msg_t *msg, void *data);


/* A cached process pid. */
nxt_pid_t  nxt_pid;

/* An original parent process pid. */
nxt_pid_t  nxt_ppid;

/* A cached process effective uid */
nxt_uid_t  nxt_euid;

/* A cached process effective gid */
nxt_gid_t  nxt_egid;

nxt_bool_t  nxt_proc_conn_matrix[NXT_PROCESS_MAX][NXT_PROCESS_MAX] = {
    { 1, 1, 1, 1, 1 },
    { 1, 0, 0, 0, 0 },
    { 1, 0, 0, 1, 0 },
    { 1, 0, 1, 0, 1 },
    { 1, 0, 0, 1, 0 },
};

nxt_bool_t  nxt_proc_remove_notify_matrix[NXT_PROCESS_MAX][NXT_PROCESS_MAX] = {
    { 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0 },
    { 0, 0, 0, 1, 0 },
    { 0, 0, 1, 0, 1 },
    { 0, 0, 0, 1, 0 },
};


nxt_process_t *
nxt_process_new(nxt_runtime_t *rt)
{
    nxt_process_t  *process;

    process = nxt_mp_zalloc(rt->mem_pool, sizeof(nxt_process_t)
                            + sizeof(nxt_process_init_t));

    if (nxt_slow_path(process == NULL)) {
        return NULL;
    }

    nxt_queue_init(&process->ports);

    nxt_thread_mutex_create(&process->incoming.mutex);

    process->use_count = 1;

    return process;
}


void
nxt_process_use(nxt_task_t *task, nxt_process_t *process, int i)
{
    process->use_count += i;

    if (process->use_count == 0) {
        nxt_runtime_process_release(task->thread->runtime, process);
    }
}


nxt_int_t
nxt_process_init_start(nxt_task_t *task, nxt_process_init_t init)
{
    nxt_int_t           ret;
    nxt_runtime_t       *rt;
    nxt_process_t       *process;
    nxt_process_init_t  *pinit;

    rt = task->thread->runtime;

    process = nxt_process_new(rt);
    if (nxt_slow_path(process == NULL)) {
        return NXT_ERROR;
    }

    process->name = init.name;
    process->user_cred = &rt->user_cred;

    pinit = nxt_process_init(process);
    *pinit = init;

    ret = nxt_process_start(task, process);
    if (nxt_slow_path(ret == NXT_ERROR)) {
        nxt_process_use(task, process, -1);
    }

    return ret;
}


nxt_int_t
nxt_process_start(nxt_task_t *task, nxt_process_t *process)
{
    nxt_mp_t            *tmp_mp;
    nxt_int_t           ret;
    nxt_pid_t           pid;
    nxt_port_t          *port;
    nxt_process_init_t  *init;

    init = nxt_process_init(process);

    port = nxt_port_new(task, 0, 0, init->type);
    if (nxt_slow_path(port == NULL)) {
        return NXT_ERROR;
    }

    nxt_process_port_add(task, process, port);

    ret = nxt_port_socket_init(task, port, 0);
    if (nxt_slow_path(ret != NXT_OK)) {
        goto free_port;
    }

    tmp_mp = nxt_mp_create(1024, 128, 256, 32);
    if (nxt_slow_path(tmp_mp == NULL)) {
        ret = NXT_ERROR;

        goto close_port;
    }

    if (init->prefork) {
        ret = init->prefork(task, process, tmp_mp);
        if (nxt_slow_path(ret != NXT_OK)) {
            goto free_mempool;
        }
    }

    pid = nxt_process_create(task, process);

    switch (pid) {

    case -1:
        ret = NXT_ERROR;
        break;

    case 0:
        /* The child process: return to the event engine work queue loop. */

        nxt_process_use(task, process, -1);

        ret = NXT_AGAIN;
        break;

    default:
        /* The main process created a new process. */

        nxt_process_use(task, process, -1);

        nxt_port_read_close(port);
        nxt_port_write_enable(task, port);

        ret = NXT_OK;
        break;
    }

free_mempool:

    nxt_mp_destroy(tmp_mp);

close_port:

    if (nxt_slow_path(ret == NXT_ERROR)) {
        nxt_port_close(task, port);
    }

free_port:

    nxt_port_use(task, port, -1);

    return ret;
}


static nxt_int_t
nxt_process_child_fixup(nxt_task_t *task, nxt_process_t *process)
{
    nxt_process_t       *p;
    nxt_runtime_t       *rt;
    nxt_process_init_t  *init;
    nxt_process_type_t  ptype;

    init = nxt_process_init(process);

    nxt_pid = nxt_getpid();

    process->pid = nxt_pid;

    /* Clean inherited cached thread tid. */
    task->thread->tid = 0;

#if (NXT_HAVE_CLONE && NXT_HAVE_CLONE_NEWPID)
    if (nxt_is_clone_flag_set(process->isolation.clone.flags, NEWPID)) {
        ssize_t  pidsz;
        char     procpid[10];

        nxt_debug(task, "%s isolated pid is %d", process->name, nxt_pid);

        pidsz = readlink("/proc/self", procpid, sizeof(procpid));

        if (nxt_slow_path(pidsz < 0 || pidsz >= (ssize_t) sizeof(procpid))) {
            nxt_alert(task, "failed to read real pid from /proc/self");
            return NXT_ERROR;
        }

        procpid[pidsz] = '\0';

        nxt_pid = (nxt_pid_t) strtol(procpid, NULL, 10);

        nxt_assert(nxt_pid >  0 && nxt_errno != ERANGE);

        process->pid = nxt_pid;
        task->thread->tid = nxt_pid;

        nxt_debug(task, "%s real pid is %d", process->name, nxt_pid);
    }

#endif

    ptype = init->type;

    nxt_port_reset_next_id();

    nxt_event_engine_thread_adopt(task->thread->engine);

    rt = task->thread->runtime;

    /* Remove not ready processes. */
    nxt_runtime_process_each(rt, p) {

        if (nxt_proc_conn_matrix[ptype][nxt_process_type(p)] == 0) {
            nxt_debug(task, "remove not required process %PI", p->pid);

            nxt_process_close_ports(task, p);

            continue;
        }

        if (p->state != NXT_PROCESS_STATE_READY) {
            nxt_debug(task, "remove not ready process %PI", p->pid);

            nxt_process_close_ports(task, p);

            continue;
        }

        nxt_port_mmaps_destroy(&p->incoming, 0);

    } nxt_runtime_process_loop;

    return NXT_OK;
}


static nxt_pid_t
nxt_process_create(nxt_task_t *task, nxt_process_t *process)
{
    nxt_int_t           ret;
    nxt_pid_t           pid;

#if (NXT_HAVE_CLONE)
    pid = nxt_clone(SIGCHLD | process->isolation.clone.flags);
    if (nxt_slow_path(pid < 0)) {
        nxt_alert(task, "clone() failed for %s %E", process->name, nxt_errno);
        return pid;
    }
#else
    pid = fork();
    if (nxt_slow_path(pid < 0)) {
        nxt_alert(task, "fork() failed for %s %E", process->name, nxt_errno);
        return pid;
    }
#endif

    if (pid == 0) {
        /* Child. */

        ret = nxt_process_child_fixup(task, process);
        if (nxt_slow_path(ret != NXT_OK)) {
            nxt_process_quit(task, 1);
            return -1;
        }

        nxt_runtime_process_add(task, process);

        if (nxt_slow_path(nxt_process_setup(task, process) != NXT_OK)) {
            nxt_process_quit(task, 1);
        }

        /*
         * Explicitly return 0 to notice the caller function this is the child.
         * The caller must return to the event engine work queue loop.
         */
        return 0;
    }

    /* Parent. */

#if (NXT_HAVE_CLONE)
    nxt_debug(task, "clone(%s): %PI", process->name, pid);
#else
    nxt_debug(task, "fork(%s): %PI", process->name, pid);
#endif

    process->pid = pid;

    nxt_runtime_process_add(task, process);

    return pid;
}


static nxt_int_t
nxt_process_setup(nxt_task_t *task, nxt_process_t *process)
{
    nxt_int_t                    ret;
    nxt_port_t                   *port, *main_port;
    nxt_thread_t                 *thread;
    nxt_runtime_t                *rt;
    nxt_process_init_t           *init;
    nxt_event_engine_t           *engine;
    const nxt_event_interface_t  *interface;

    init = nxt_process_init(process);

    nxt_debug(task, "%s setup", process->name);

    nxt_process_title(task, "unit: %s", process->name);

    thread = task->thread;
    rt     = thread->runtime;

    nxt_random_init(&thread->random);

    rt->type = init->type;

    engine = thread->engine;

    /* Update inherited main process event engine and signals processing. */
    engine->signals->sigev = init->signals;

    interface = nxt_service_get(rt->services, "engine", rt->engine);
    if (nxt_slow_path(interface == NULL)) {
        return NXT_ERROR;
    }

    if (nxt_event_engine_change(engine, interface, rt->batch) != NXT_OK) {
        return NXT_ERROR;
    }

    ret = nxt_runtime_thread_pool_create(thread, rt, rt->auxiliary_threads,
                                         60000 * 1000000LL);
    if (nxt_slow_path(ret != NXT_OK)) {
        return NXT_ERROR;
    }

    main_port = rt->port_by_type[NXT_PROCESS_MAIN];

    nxt_port_read_close(main_port);
    nxt_port_write_enable(task, main_port);

    port = nxt_process_port_first(process);

    nxt_port_enable(task, port, init->port_handlers);

    ret = init->setup(task, process);

    if (nxt_slow_path(ret != NXT_OK)) {
        return NXT_ERROR;
    }

    switch (process->state) {

    case NXT_PROCESS_STATE_CREATED:
        ret = nxt_process_send_created(task, process);
        break;

    case NXT_PROCESS_STATE_READY:
        ret = nxt_process_send_ready(task, process);

        if (nxt_slow_path(ret != NXT_OK)) {
            break;
        }

        ret = init->start(task, &process->data);

        nxt_port_write_close(port);

        break;

    default:
        nxt_assert(0);
    }

    if (nxt_slow_path(ret != NXT_OK)) {
        nxt_alert(task, "%s failed to start", process->name);
    }

    return ret;
}


static nxt_int_t
nxt_process_send_created(nxt_task_t *task, nxt_process_t *process)
{
    uint32_t            stream;
    nxt_int_t           ret;
    nxt_port_t          *my_port, *main_port;
    nxt_runtime_t       *rt;

    nxt_assert(process->state == NXT_PROCESS_STATE_CREATED);

    rt = task->thread->runtime;

    my_port = nxt_process_port_first(process);
    main_port = rt->port_by_type[NXT_PROCESS_MAIN];

    nxt_assert(my_port != NULL && main_port != NULL);

    stream = nxt_port_rpc_register_handler(task, my_port,
                                           nxt_process_created_ok,
                                           nxt_process_created_error,
                                           main_port->pid, process);

    if (nxt_slow_path(stream == 0)) {
        return NXT_ERROR;
    }

    ret = nxt_port_socket_write(task, main_port, NXT_PORT_MSG_PROCESS_CREATED,
                                -1, stream, my_port->id, NULL);

    if (nxt_slow_path(ret != NXT_OK)) {
        nxt_alert(task, "%s failed to send CREATED message", process->name);
        nxt_port_rpc_cancel(task, my_port, stream);
        return NXT_ERROR;
    }

    nxt_debug(task, "%s created", process->name);

    return NXT_OK;
}


static void
nxt_process_created_ok(nxt_task_t *task, nxt_port_recv_msg_t *msg, void *data)
{
    nxt_int_t           ret;
    nxt_process_t       *process;
    nxt_process_init_t  *init;

    process = data;
    init = nxt_process_init(process);

    ret = nxt_process_apply_creds(task, process);
    if (nxt_slow_path(ret != NXT_OK)) {
        goto fail;
    }

    nxt_log(task, NXT_LOG_INFO, "%s started", process->name);

    ret = init->start(task, &process->data);

fail:

    nxt_process_quit(task, ret == NXT_OK ? 0 : 1);
}


static void
nxt_process_created_error(nxt_task_t *task, nxt_port_recv_msg_t *msg,
    void *data)
{
    nxt_process_t       *process;
    nxt_process_init_t  *init;

    process = data;
    init = nxt_process_init(process);

    nxt_alert(task, "%s failed to start", init->name);

    nxt_process_quit(task, 1);
}


nxt_int_t
nxt_process_core_setup(nxt_task_t *task, nxt_process_t *process)
{
    nxt_int_t  ret;

    ret = nxt_process_apply_creds(task, process);
    if (nxt_slow_path(ret != NXT_OK)) {
        return NXT_ERROR;
    }

    process->state = NXT_PROCESS_STATE_READY;

    return NXT_OK;
}


nxt_int_t
nxt_process_creds_set(nxt_task_t *task, nxt_process_t *process, nxt_str_t *user,
    nxt_str_t *group)
{
    char  *str;

    process->user_cred = nxt_mp_zalloc(process->mem_pool,
                                       sizeof(nxt_credential_t));

    if (nxt_slow_path(process->user_cred == NULL)) {
        return NXT_ERROR;
    }

    str = nxt_mp_zalloc(process->mem_pool, user->length + 1);
    if (nxt_slow_path(str == NULL)) {
        return NXT_ERROR;
    }

    nxt_memcpy(str, user->start, user->length);
    str[user->length] = '\0';

    process->user_cred->user = str;

    if (group->start != NULL) {
        str = nxt_mp_zalloc(process->mem_pool, group->length + 1);
        if (nxt_slow_path(str == NULL)) {
            return NXT_ERROR;
        }

        nxt_memcpy(str, group->start, group->length);
        str[group->length] = '\0';

    } else {
        str = NULL;
    }

    return nxt_credential_get(task, process->mem_pool, process->user_cred, str);
}


nxt_int_t
nxt_process_apply_creds(nxt_task_t *task, nxt_process_t *process)
{
    nxt_int_t      ret, cap_setid;
    nxt_runtime_t  *rt;

    rt = task->thread->runtime;

    cap_setid = rt->capabilities.setid;

#if (NXT_HAVE_CLONE && NXT_HAVE_CLONE_NEWUSER)
    if (!cap_setid
        && nxt_is_clone_flag_set(process->isolation.clone.flags, NEWUSER)) {
        cap_setid = 1;
    }
#endif

    if (cap_setid) {
        ret = nxt_credential_setgids(task, process->user_cred);
        if (nxt_slow_path(ret != NXT_OK)) {
            return NXT_ERROR;
        }

        ret = nxt_credential_setuid(task, process->user_cred);
        if (nxt_slow_path(ret != NXT_OK)) {
            return NXT_ERROR;
        }
    }

#if (NXT_HAVE_PR_SET_NO_NEW_PRIVS)
    if (nxt_slow_path(process->isolation.new_privs == 0
                      && prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0))
    {
        nxt_alert(task, "failed to set no_new_privs %E", nxt_errno);
        return NXT_ERROR;
    }
#endif

    return NXT_OK;
}


static nxt_int_t
nxt_process_send_ready(nxt_task_t *task, nxt_process_t *process)
{
    nxt_int_t           ret;
    nxt_port_t          *main_port;
    nxt_runtime_t       *rt;

    rt = task->thread->runtime;

    main_port = rt->port_by_type[NXT_PROCESS_MAIN];

    nxt_assert(main_port != NULL);

    ret = nxt_port_socket_write(task, main_port, NXT_PORT_MSG_PROCESS_READY,
                                -1, process->stream, 0, NULL);

    if (nxt_slow_path(ret != NXT_OK)) {
        nxt_alert(task, "%s failed to send READY message", process->name);
        return NXT_ERROR;
    }

    nxt_debug(task, "%s sent ready", process->name);

    return NXT_OK;
}


#if (NXT_HAVE_POSIX_SPAWN)

/*
 * Linux glibc 2.2 posix_spawn() is implemented via fork()/execve().
 * Linux glibc 2.4 posix_spawn() without file actions and spawn
 * attributes uses vfork()/execve().
 *
 * On FreeBSD 8.0 posix_spawn() is implemented via vfork()/execve().
 *
 * Solaris 10:
 *   In the Solaris 10 OS, posix_spawn() is currently implemented using
 *   private-to-libc vfork(), execve(), and exit() functions.  They are
 *   identical to regular vfork(), execve(), and exit() in functionality,
 *   but they are not exported from libc and therefore don't cause the
 *   deadlock-in-the-dynamic-linker problem that any multithreaded code
 *   outside of libc that calls vfork() can cause.
 *
 * On MacOSX 10.5 (Leoprad) and NetBSD 6.0 posix_spawn() is implemented
 * as syscall.
 */

nxt_pid_t
nxt_process_execute(nxt_task_t *task, char *name, char **argv, char **envp)
{
    nxt_pid_t  pid;

    nxt_debug(task, "posix_spawn(\"%s\")", name);

    if (posix_spawn(&pid, name, NULL, NULL, argv, envp) != 0) {
        nxt_alert(task, "posix_spawn(\"%s\") failed %E", name, nxt_errno);
        return -1;
    }

    return pid;
}

#else

nxt_pid_t
nxt_process_execute(nxt_task_t *task, char *name, char **argv, char **envp)
{
    nxt_pid_t  pid;

    /*
     * vfork() is better than fork() because:
     *   it is faster several times;
     *   its execution time does not depend on private memory mapping size;
     *   it has lesser chances to fail due to the ENOMEM error.
     */

    pid = vfork();

    switch (pid) {

    case -1:
        nxt_alert(task, "vfork() failed while executing \"%s\" %E",
                  name, nxt_errno);
        break;

    case 0:
        /* A child. */
        nxt_debug(task, "execve(\"%s\")", name);

        (void) execve(name, argv, envp);

        nxt_alert(task, "execve(\"%s\") failed %E", name, nxt_errno);

        exit(1);
        nxt_unreachable();
        break;

    default:
        /* A parent. */
        nxt_debug(task, "vfork(): %PI", pid);
        break;
    }

    return pid;
}

#endif


nxt_int_t
nxt_process_daemon(nxt_task_t *task)
{
    nxt_fd_t      fd;
    nxt_pid_t     pid;
    const char    *msg;

    fd = -1;

    /*
     * fork() followed by a parent process's exit() detaches a child process
     * from an init script or terminal shell process which has started the
     * parent process and allows the child process to run in background.
     */

    pid = fork();

    switch (pid) {

    case -1:
        msg = "fork() failed %E";
        goto fail;

    case 0:
        /* A child. */
        break;

    default:
        /* A parent. */
        nxt_debug(task, "fork(): %PI", pid);
        exit(0);
        nxt_unreachable();
    }

    nxt_pid = getpid();

    /* Clean inherited cached thread tid. */
    task->thread->tid = 0;

    nxt_debug(task, "daemon");

    /* Detach from controlling terminal. */

    if (setsid() == -1) {
        nxt_alert(task, "setsid() failed %E", nxt_errno);
        return NXT_ERROR;
    }

    /*
     * Reset file mode creation mask: any access
     * rights can be set on file creation.
     */
    umask(0);

    /* Redirect STDIN and STDOUT to the "/dev/null". */

    fd = open("/dev/null", O_RDWR);
    if (fd == -1) {
        msg = "open(\"/dev/null\") failed %E";
        goto fail;
    }

    if (dup2(fd, STDIN_FILENO) == -1) {
        msg = "dup2(\"/dev/null\", STDIN) failed %E";
        goto fail;
    }

    if (dup2(fd, STDOUT_FILENO) == -1) {
        msg = "dup2(\"/dev/null\", STDOUT) failed %E";
        goto fail;
    }

    if (fd > STDERR_FILENO) {
        nxt_fd_close(fd);
    }

    return NXT_OK;

fail:

    nxt_alert(task, msg, nxt_errno);

    if (fd != -1) {
        nxt_fd_close(fd);
    }

    return NXT_ERROR;
}


void
nxt_nanosleep(nxt_nsec_t ns)
{
    struct timespec  ts;

    ts.tv_sec = ns / 1000000000;
    ts.tv_nsec = ns % 1000000000;

    (void) nanosleep(&ts, NULL);
}


void
nxt_process_port_add(nxt_task_t *task, nxt_process_t *process, nxt_port_t *port)
{
    nxt_assert(port->process == NULL);

    port->process = process;
    nxt_queue_insert_tail(&process->ports, &port->link);

    nxt_process_use(task, process, 1);
}


nxt_process_type_t
nxt_process_type(nxt_process_t *process)
{
    return nxt_queue_is_empty(&process->ports) ? 0 :
        (nxt_process_port_first(process))->type;
}


void
nxt_process_close_ports(nxt_task_t *task, nxt_process_t *process)
{
    nxt_port_t  *port;

    nxt_process_port_each(process, port) {

        nxt_port_close(task, port);

        nxt_runtime_port_remove(task, port);

    } nxt_process_port_loop;
}


void
nxt_process_quit(nxt_task_t *task, nxt_uint_t exit_status)
{
    nxt_uint_t           n;
    nxt_queue_t          *listen;
    nxt_runtime_t        *rt;
    nxt_queue_link_t     *link, *next;
    nxt_listen_event_t   *lev;
    nxt_listen_socket_t  *ls;

    rt = task->thread->runtime;

    nxt_debug(task, "close listen connections");

    listen = &task->thread->engine->listen_connections;

    for (link = nxt_queue_first(listen);
         link != nxt_queue_tail(listen);
         link = next)
    {
        next = nxt_queue_next(link);
        lev = nxt_queue_link_data(link, nxt_listen_event_t, link);
        nxt_queue_remove(link);

        nxt_fd_event_close(task->thread->engine, &lev->socket);
    }

    if (rt->listen_sockets != NULL) {

        ls = rt->listen_sockets->elts;
        n = rt->listen_sockets->nelts;

        while (n != 0) {
            nxt_socket_close(task, ls->socket);
            ls->socket = -1;

            ls++;
            n--;
        }

        rt->listen_sockets->nelts = 0;
    }

    nxt_runtime_quit(task, exit_status);
}
