/*
    SSSD

    Kerberos 5 Backend Module

    Authors:
        Sumit Bose <sbose@redhat.com>

    Copyright (C) 2009 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include <errno.h>
#include <sys/time.h>
#include <krb5/krb5.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>

#include <security/pam_modules.h>

#include "util/util.h"
#include "providers/dp_backend.h"
#include "db/sysdb.h"
#include "krb5_plugin/sssd_krb5_locator_plugin.h"
#include "providers/krb5/krb5_auth.h"

#ifndef SSSD_LIBEXEC_PATH
#error "SSSD_LIBEXEC_PATH not defined"
#else
#define KRB5_CHILD SSSD_LIBEXEC_PATH"/krb5_child"
#endif

struct krb5child_req {
    pid_t child_pid;
    int read_from_child_fd;
    int write_to_child_fd;

    struct be_req *req;
    struct pam_data *pd;
    struct krb5_ctx *krb5_ctx;
};

static errno_t become_user(uid_t uid, gid_t gid)
{
    int ret;
    ret = setgid(gid);
    if (ret == -1) {
        DEBUG(1, ("setgid failed [%d][%s].\n", errno, strerror(errno)));
        return errno;
    }

    ret = setuid(uid);
    if (ret == -1) {
        DEBUG(1, ("setuid failed [%d][%s].\n", errno, strerror(errno)));
        return errno;
    }

    ret = setegid(gid);
    if (ret == -1) {
        DEBUG(1, ("setegid failed [%d][%s].\n", errno, strerror(errno)));
        return errno;
    }

    ret = seteuid(uid);
    if (ret == -1) {
        DEBUG(1, ("seteuid failed [%d][%s].\n", errno, strerror(errno)));
        return errno;
    }

    return EOK;
}

struct io_buffer {
    uint8_t *data;
    size_t size;
};

errno_t create_send_buffer(struct krb5child_req *kr, struct io_buffer **io_buf)
{
    struct io_buffer *buf;
    size_t rp;

    buf = talloc(kr, struct io_buffer);
    if (buf == NULL) {
        DEBUG(1, ("talloc failed.\n"));
        return ENOMEM;
    }

    buf->size = 3*sizeof(int) + strlen(kr->pd->upn) + kr->pd->authtok_size;
    if (kr->pd->cmd == SSS_PAM_CHAUTHTOK) {
        buf->size += sizeof(int) + kr->pd->newauthtok_size;
    }

    buf->data = talloc_size(kr, buf->size);
    if (buf->data == NULL) {
        DEBUG(1, ("talloc_size failed.\n"));
        talloc_free(buf);
        return ENOMEM;
    }

    rp = 0;
    ((uint32_t *)(&buf->data[rp]))[0] = kr->pd->cmd;
    rp += sizeof(uint32_t);

    ((uint32_t *)(&buf->data[rp]))[0] = strlen(kr->pd->upn);
    rp += sizeof(uint32_t);

    memcpy(&buf->data[rp], kr->pd->upn, strlen(kr->pd->upn));
    rp += strlen(kr->pd->upn);

    ((uint32_t *)(&buf->data[rp]))[0] = kr->pd->authtok_size;
    rp += sizeof(uint32_t);

    memcpy(&buf->data[rp], kr->pd->authtok, kr->pd->authtok_size);
    rp += kr->pd->authtok_size;

    if (kr->pd->cmd == SSS_PAM_CHAUTHTOK) {
        ((uint32_t *)(&buf->data[rp]))[0] = kr->pd->newauthtok_size;
        rp += sizeof(uint32_t);

        memcpy(&buf->data[rp], kr->pd->newauthtok, kr->pd->newauthtok_size);
        rp += kr->pd->newauthtok_size;
    }

    *io_buf = buf;

    return EOK;
}

static void fd_nonblocking(int fd) {
    int flags;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        DEBUG(1, ("F_GETFL failed [%d][%s].\n", errno, strerror(errno)));
        return;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        DEBUG(1, ("F_SETFL failed [%d][%s].\n", errno, strerror(errno)));
    }

    return;
}

static void krb5_cleanup(struct krb5child_req *kr)
{
    if (kr == NULL) return;

    memset(kr, 0, sizeof(struct krb5child_req));

    talloc_zfree(kr);
}

static errno_t krb5_setup(struct be_req *req, struct krb5child_req **krb5_req)
{
    struct krb5child_req *kr = NULL;
    struct krb5_ctx *krb5_ctx;
    struct pam_data *pd;
    errno_t err;

    pd = talloc_get_type(req->req_data, struct pam_data);

    krb5_ctx = talloc_get_type(req->be_ctx->bet_info[BET_AUTH].pvt_bet_data,
                               struct krb5_ctx);

    kr = talloc_zero(req, struct krb5child_req);
    if (kr == NULL) {
        DEBUG(1, ("talloc failed.\n"));
        err = ENOMEM;
        goto failed;
    }

    kr->pd = pd;
    kr->req = req;
    kr->krb5_ctx = krb5_ctx;

    *krb5_req = kr;

    return EOK;

failed:
    krb5_cleanup(kr);

    return err;
}

static void wait_for_child_handler(struct tevent_context *ev,
                                struct tevent_signal *sige, int signum,
                                int count, void *__siginfo, void *pvt)
{
    int ret;
    int child_status;
    siginfo_t *siginfo = (siginfo_t *)__siginfo;

    errno = 0;
    do {
        ret = waitpid(siginfo->si_pid, &child_status, WNOHANG);
    } while (ret == -1 && errno == EINTR);
    if (ret == siginfo->si_pid) {
        DEBUG(4, ("child status [%d].\n", child_status));
        if (WEXITSTATUS(child_status) != 0) {
            DEBUG(1, ("child failed.\n"));
        }
    } else if (ret == 0) {
        DEBUG(1, ("waitpid did not found a child with changed status.\n", ret));
    } else if (ret >= 0 && ret != siginfo->si_pid) {
        DEBUG(1, ("waitpid returned wrong child pid [%d], continue waiting.\n", ret));
    } else if (ret == -1 && errno == ECHILD) {
        DEBUG(1, ("no child with pid [%d].\n", siginfo->si_pid));
    } else {
        DEBUG(1, ("waitpid failed [%s].\n", strerror(errno)));
    }

    return;
}

static errno_t fork_child(struct krb5child_req *kr)
{
    int pipefd_to_child[2];
    int pipefd_from_child[2];
    pid_t pid;
    int ret;
    errno_t err;

    ret = pipe(pipefd_from_child);
    if (ret == -1) {
        err = errno;
        DEBUG(1, ("pipe failed [%d][%s].\n", errno, strerror(errno)));
        return err;
    }
    ret = pipe(pipefd_to_child);
    if (ret == -1) {
        err = errno;
        DEBUG(1, ("pipe failed [%d][%s].\n", errno, strerror(errno)));
        return err;
    }

    pid = fork();

    if (pid == 0) { /* child */
        //talloc_free(kr->req->be_ctx->ev);

        ret = chdir("/tmp");
        if (ret == -1) {
            err = errno;
            DEBUG(1, ("chdir failed [%d][%s].\n", errno, strerror(errno)));
            return err;
        }

        ret = become_user(kr->pd->pw_uid, kr->pd->gr_gid);
        if (ret != EOK) {
            DEBUG(1, ("become_user failed.\n"));
            return ret;
        }


        close(pipefd_to_child[1]);
        ret = dup2(pipefd_to_child[0],STDIN_FILENO);
        if (ret == -1) {
            err = errno;
            DEBUG(1, ("dup2 failed [%d][%s].\n", errno, strerror(errno)));
            return err;
        }

        close(pipefd_from_child[0]);
        ret = dup2(pipefd_from_child[1],STDOUT_FILENO);
        if (ret == -1) {
            err = errno;
            DEBUG(1, ("dup2 failed [%d][%s].\n", errno, strerror(errno)));
            return err;
        }

        ret = execl(KRB5_CHILD, KRB5_CHILD, NULL);
        if (ret == -1) {
            err = errno;
            DEBUG(1, ("execl failed [%d][%s].\n", errno, strerror(errno)));
            return err;
        }
    } else if (pid > 0) { /* parent */
        kr->child_pid = pid;
        kr->read_from_child_fd = pipefd_from_child[0];
        close(pipefd_from_child[1]);
        kr->write_to_child_fd = pipefd_to_child[1];
        close(pipefd_to_child[0]);
        fd_nonblocking(kr->read_from_child_fd);
        fd_nonblocking(kr->write_to_child_fd);

    } else { /* error */
        err = errno;
        DEBUG(1, ("fork failed [%d][%s].\n", errno, strerror(errno)));
        return err;
    }

    return EOK;
}


struct read_pipe_state {
    int fd;
    uint8_t *buf;
    size_t len;
};

static void read_pipe_done(struct tevent_context *ev, struct tevent_fd *fde,
                         uint16_t flags, void *pvt);

static struct tevent_req *read_pipe_send(TALLOC_CTX *memctx,
                                       struct tevent_context *ev,
                                       int fd)
{
    struct tevent_req *req;
    struct read_pipe_state *state;
    struct tevent_fd *fde;


    req = tevent_req_create(memctx, &state, struct read_pipe_state);
    if (req == NULL) return NULL;

    state->fd = fd;
    state->buf = talloc_array(state, uint8_t, MAX_CHILD_MSG_SIZE);
    if (state->buf == NULL) goto fail;

    fde = tevent_add_fd(ev, state, fd, TEVENT_FD_READ,
                       read_pipe_done, req);
    if (fde == NULL) {
        DEBUG(1, ("tevent_add_fd failed.\n"));
        goto fail;
    }

    return req;

fail:
    talloc_zfree(req);
    return NULL;
}

static void read_pipe_done(struct tevent_context *ev, struct tevent_fd *fde,
                         uint16_t flags, void *pvt)
{
    ssize_t size;
    struct tevent_req *req = talloc_get_type(pvt, struct tevent_req);
    struct read_pipe_state *state = tevent_req_data(req, struct read_pipe_state);

    if (flags & TEVENT_FD_WRITE) {
        DEBUG(1, ("client_response_handler called with TEVENT_FD_WRITE, this should not happen.\n"));
        tevent_req_error(req, EINVAL);
        return;
    }

    size = read(state->fd, state->buf, talloc_get_size(state->buf));
    if (size == -1) {
        if (errno == EAGAIN || errno == EINTR) return;
        DEBUG(1, ("read failed [%d][%s].\n", errno, strerror(errno)));
        tevent_req_error(req, errno);
        return;
    }
    state->len = size;

    tevent_req_done(req);
    return;
}

static ssize_t read_pipe_recv(struct tevent_req *req, TALLOC_CTX *mem_ctx,
                        uint8_t **buf, uint64_t *error) {
    struct read_pipe_state *state = tevent_req_data(req,
                                                    struct read_pipe_state);
    enum tevent_req_state tstate;

    if (tevent_req_is_error(req, &tstate, error)) {
        return -1;
    }

    *buf = talloc_move(mem_ctx, &state->buf);
    return state->len;
}

struct handle_child_state {
    struct krb5child_req *kr;
    ssize_t len;
    uint8_t *buf;
};

static void handle_child_done(struct tevent_req *subreq);

static struct tevent_req *handle_child_send(TALLOC_CTX *mem_ctx, struct tevent_context *ev,
                        struct krb5child_req *kr)
{
    int ret;
    struct tevent_req *req;
    struct tevent_req *subreq;
    struct handle_child_state *state;
    struct io_buffer *buf;

    ret = create_send_buffer(kr, &buf);
    if (ret != EOK) {
        DEBUG(1, ("create_send_buffer failed.\n"));
        return NULL;
    }

    ret = fork_child(kr);
    if (ret != EOK) {
        DEBUG(1, ("fork_child failed.\n"));
        return NULL;
    }

    ret = write(kr->write_to_child_fd, buf->data, buf->size);
    close(kr->write_to_child_fd);
    if (ret == -1) {
        DEBUG(1, ("write failed [%d][%s].\n", errno, strerror(errno)));
        return NULL;
    }

    req = tevent_req_create(mem_ctx, &state, struct handle_child_state);
    if (req == NULL) {
        return NULL;
    }

    state->kr = kr;

    subreq = read_pipe_send(state, ev, kr->read_from_child_fd);
    if (tevent_req_nomem(subreq, req)) {
        return tevent_req_post(req, ev);
    }
    tevent_req_set_callback(subreq, handle_child_done, req);
    return req;
}

static void handle_child_done(struct tevent_req *subreq)
{
    struct tevent_req *req = tevent_req_callback_data(subreq,
                                                      struct tevent_req);
    struct handle_child_state *state = tevent_req_data(req,
                                                    struct handle_child_state);
    uint64_t error;

    state->len = read_pipe_recv(subreq, state, &state->buf, &error);
    talloc_zfree(subreq);
    close(state->kr->read_from_child_fd);
    if (state->len == -1) {
        tevent_req_error(req, error);
        return;
    }

    tevent_req_done(req);
    return;
}

static ssize_t handle_child_recv(struct tevent_req *req, TALLOC_CTX *mem_ctx,
                     uint8_t **buf, uint64_t *error) {
    struct handle_child_state *state = tevent_req_data(req,
                                                    struct handle_child_state);
    enum tevent_req_state tstate;

    if (tevent_req_is_error(req, &tstate, error)) {
        return -1;
    }

    *buf = talloc_move(mem_ctx, &state->buf);
    return state->len;
}

static void get_user_upn_done(void *pvt, int err, struct ldb_result *res);
static void krb5_pam_handler_done(struct tevent_req *req);
static void krb5_pam_handler_cache_done(struct tevent_req *treq);

static void krb5_pam_handler(struct be_req *be_req)
{
    int ret;
    struct pam_data *pd;
    int pam_status=PAM_SYSTEM_ERR;
    const char **attrs;

    pd = talloc_get_type(be_req->req_data, struct pam_data);

    if (be_is_offline(be_req->be_ctx)) {
        DEBUG(4, ("Backend is marked offline, retry later!\n"));
        pam_status = PAM_AUTHINFO_UNAVAIL;
        goto done;
    }

    if (pd->cmd != SSS_PAM_AUTHENTICATE && pd->cmd != SSS_PAM_CHAUTHTOK) {
        DEBUG(4, ("krb5 does not handles pam task %d.\n", pd->cmd));
        pam_status = PAM_SUCCESS;
        goto done;
    }

    attrs = talloc_array(be_req, const char *, 2);
    if (attrs == NULL) {
        goto done;
    }

    attrs[0] = SYSDB_UPN;
    attrs[1] = NULL;

    ret = sysdb_get_user_attr(be_req, be_req->be_ctx->sysdb,
                              be_req->be_ctx->domain, pd->user, attrs,
                              get_user_upn_done, be_req);

    if (ret) {
        goto done;
    }

    return;

done:
    pd->pam_status = pam_status;

    be_req->fn(be_req, pam_status, NULL);
}

static void get_user_upn_done(void *pvt, int err, struct ldb_result *res)
{
    struct be_req *be_req = talloc_get_type(pvt, struct be_req);
    struct krb5_ctx *krb5_ctx;
    struct krb5child_req *kr = NULL;
    struct tevent_req *req;
    int ret;
    struct pam_data *pd;
    int pam_status=PAM_SYSTEM_ERR;
    //const char *upn = NULL;

    pd = talloc_get_type(be_req->req_data, struct pam_data);
    krb5_ctx = talloc_get_type(be_req->be_ctx->bet_info[BET_AUTH].pvt_bet_data,
                               struct krb5_ctx);

    if (err != LDB_SUCCESS) {
        DEBUG(5, ("sysdb search for upn of user [%s] failed.\n", pd->user));
        goto failed;
    }

    switch (res->count) {
    case 0:
        DEBUG(5, ("No upn for user [%s] found.\n", pd->user));
        break;

    case 1:
        pd->upn = ldb_msg_find_attr_as_string(res->msgs[0], SYSDB_UPN, NULL);
        if (pd->upn == NULL && krb5_ctx->try_simple_upn) {
            /* NOTE: this is a hack, works only in some environments */
            if (krb5_ctx->realm != NULL) {
                pd->upn = talloc_asprintf(be_req, "%s@%s", pd->user,
                                      krb5_ctx->realm);
                if (pd->upn == NULL) {
                    DEBUG(1, ("failed to build simple upn.\n"));
                }
                DEBUG(9, ("Using simple UPN [%s].\n", pd->upn));
            }
        }
        break;

    default:
        DEBUG(1, ("A user search by name (%s) returned > 1 results!\n",
                  pd->user));
        break;
    }

    if (pd->upn == NULL) {
        DEBUG(1, ("Cannot set UPN.\n"));
        goto failed;
    }

    ret = krb5_setup(be_req, &kr);
    if (ret != EOK) {
        DEBUG(1, ("krb5_setup failed.\n"));
        goto failed;
    }

    req = handle_child_send(be_req, be_req->be_ctx->ev, kr);
    if (req == NULL) {
        DEBUG(1, ("handle_child_send failed.\n"));
        goto failed;
    }

    tevent_req_set_callback(req, krb5_pam_handler_done, kr);
    return;

failed:
    krb5_cleanup(kr);

    pd->pam_status = pam_status;

    be_req->fn(be_req, pam_status, NULL);
}

static void krb5_pam_handler_done(struct tevent_req *req)
{
    struct krb5child_req *kr = tevent_req_callback_data(req,
                                                        struct krb5child_req);
    struct pam_data *pd = kr->pd;
    struct be_req *be_req = kr->req;
    struct krb5_ctx *krb5_ctx = kr->krb5_ctx;
    int ret;
    uint8_t *buf;
    ssize_t len;
    uint64_t error;
    int p;
    int32_t *msg_status;
    int32_t *msg_type;
    int32_t *msg_len;
    struct tevent_req *subreq = NULL;
    char *password = NULL;
    char *env = NULL;

    pd->pam_status = PAM_SYSTEM_ERR;
    krb5_cleanup(kr);

    len = handle_child_recv(req, pd, &buf, &error);
    talloc_zfree(req);
    if (len == -1) {
        DEBUG(1, ("child failed\n"));
        goto done;
    }

    if ((size_t) len < 3*sizeof(int32_t)) {
        DEBUG(1, ("message too short.\n"));
        goto done;
    }

    p=0;
    msg_status = ((int32_t *)(buf+p));
    p += sizeof(int32_t);

    msg_type = ((int32_t *)(buf+p));
    p += sizeof(int32_t);

    msg_len = ((int32_t *)(buf+p));
    p += sizeof(int32_t);

    DEBUG(4, ("child response [%d][%d][%d].\n", *msg_status, *msg_type,
                                                *msg_len));

    if ((p + *msg_len) != len) {
        DEBUG(1, ("message format error.\n"));
        goto done;
    }

    ret=pam_add_response(pd, *msg_type, *msg_len, &buf[p]);
    if (ret != EOK) {
        DEBUG(1, ("pam_add_response failed.\n"));
        goto done;
    }

    pd->pam_status = *msg_status;

    if (pd->pam_status == PAM_AUTHINFO_UNAVAIL) {
        be_mark_offline(be_req->be_ctx);
        goto done;
    }

    if (pd->pam_status == PAM_SUCCESS && pd->cmd == SSS_PAM_AUTHENTICATE) {
        env = talloc_asprintf(pd, "%s=%s", SSSD_REALM, krb5_ctx->realm);
        if (env == NULL) {
            DEBUG(1, ("talloc_asprintf failed.\n"));
            goto done;
        }
        ret=pam_add_response(pd, PAM_ENV_ITEM, strlen(env)+1, (uint8_t *) env);
        if (ret != EOK) {
            DEBUG(1, ("pam_add_response failed.\n"));
            goto done;
        }

        env = talloc_asprintf(pd, "%s=%s", SSSD_KDC, krb5_ctx->kdcip);
        if (env == NULL) {
            DEBUG(1, ("talloc_asprintf failed.\n"));
            goto done;
        }
        ret=pam_add_response(pd, PAM_ENV_ITEM, strlen(env)+1, (uint8_t *) env);
        if (ret != EOK) {
            DEBUG(1, ("pam_add_response failed.\n"));
            goto done;
        }
    }

    if (pd->pam_status == PAM_SUCCESS &&
        be_req->be_ctx->domain->cache_credentials == TRUE) {

        switch(pd->cmd) {
            case SSS_PAM_AUTHENTICATE:
                password = talloc_size(be_req, pd->authtok_size + 1);
                if (password != NULL) {
                    memcpy(password, pd->authtok, pd->authtok_size);
                    password[pd->authtok_size] = '\0';
                }
                break;
            case SSS_PAM_CHAUTHTOK:
                password = talloc_size(be_req, pd->newauthtok_size + 1);
                if (password != NULL) {
                    memcpy(password, pd->newauthtok, pd->newauthtok_size);
                    password[pd->newauthtok_size] = '\0';
                }
                break;
            default:
                DEBUG(0, ("unsupported PAM command [%d].\n", pd->cmd));
        }

        if (password == NULL) {
            DEBUG(0, ("password not available, offline auth may not work.\n"));
            goto done;
        }

        talloc_set_destructor((TALLOC_CTX *)password, password_destructor);

        subreq = sysdb_cache_password_send(be_req, be_req->be_ctx->ev,
                                           be_req->be_ctx->sysdb, NULL,
                                           be_req->be_ctx->domain, pd->user,
                                           password);
        if (subreq == NULL) {
            DEBUG(2, ("cache_password_send failed, offline auth may not work.\n"));
            goto done;
        }
        tevent_req_set_callback(subreq, krb5_pam_handler_cache_done, be_req);

        return;
    }
done:
    be_req->fn(be_req, pd->pam_status, NULL);
}

static void krb5_pam_handler_cache_done(struct tevent_req *subreq)
{
    struct be_req *be_req = tevent_req_callback_data(subreq, struct be_req);
    int ret;

    /* password caching failures are not fatal errors */
    ret = sysdb_cache_password_recv(subreq);
    talloc_zfree(subreq);

    /* so we just log it any return */
    if (ret) {
        DEBUG(2, ("Failed to cache password (%d)[%s]!?\n",
                  ret, strerror(ret)));
    }

    be_req->fn(be_req, PAM_SUCCESS, NULL);
}

struct bet_ops krb5_auth_ops = {
    .handler = krb5_pam_handler,
    .finalize = NULL,
};

struct bet_ops krb5_chpass_ops = {
    .handler = krb5_pam_handler,
    .finalize = NULL,
};


int sssm_krb5_auth_init(struct be_ctx *bectx,
                        struct bet_ops **ops, void **pvt_auth_data)
{
    struct krb5_ctx *ctx = NULL;
    char *value = NULL;
    bool bool_value;
    int ret;
    struct tevent_signal *sige;

    ctx = talloc_zero(bectx, struct krb5_ctx);
    if (!ctx) {
        DEBUG(1, ("talloc failed.\n"));
        return ENOMEM;
    }

    ctx->action = INIT_PW;

    ret = confdb_get_string(bectx->cdb, ctx, bectx->conf_path,
                            "krb5KDCIP", NULL, &value);
    if (ret != EOK) goto fail;
    if (value == NULL) {
        DEBUG(2, ("Missing krb5KDCIP, authentication might fail.\n"));
    } else {
        ret = setenv(SSSD_KDC, value, 1);
        if (ret != EOK) {
            DEBUG(2, ("setenv %s failed, authentication might fail.\n",
                      SSSD_KDC));
        }
    }
    ctx->kdcip = value;

    ret = confdb_get_string(bectx->cdb, ctx, bectx->conf_path,
                            "krb5REALM", NULL, &value);
    if (ret != EOK) goto fail;
    if (value == NULL) {
        DEBUG(4, ("Missing krb5REALM authentication might fail.\n"));
    } else {
        ret = setenv(SSSD_REALM, value, 1);
        if (ret != EOK) {
            DEBUG(2, ("setenv %s failed, authentication might fail.\n",
                      SSSD_REALM));
        }
    }
    ctx->realm = value;

    ret = confdb_get_bool(bectx->cdb, ctx, bectx->conf_path,
                          "krb5try_simple_upn", false, &bool_value);
    if (ret != EOK) goto fail;
    ctx->try_simple_upn = bool_value;

    ret = confdb_get_string(bectx->cdb, ctx, bectx->conf_path,
                            "krb5changepw_principle", "kadmin/changepw",
                            &value);
    if (ret != EOK) goto fail;
    if (strchr(value, '@') == NULL) {
        value = talloc_asprintf_append(value, "@%s", ctx->realm);
        if (value == NULL) {
            DEBUG(7, ("talloc_asprintf_append failed.\n"));
            goto fail;
        }
    }
    ctx->changepw_principle = value;

    ret = setenv(SSSD_KRB5_CHANGEPW_PRINCIPLE, ctx->changepw_principle, 1);
    if (ret != EOK) {
        DEBUG(2, ("setenv %s failed, password change might fail.\n",
                  SSSD_KRB5_CHANGEPW_PRINCIPLE));
    }

/* TODO: set options */

    sige = tevent_add_signal(bectx->ev, ctx, SIGCHLD, SA_SIGINFO,
                       wait_for_child_handler, NULL);
    if (sige == NULL) {
        DEBUG(1, ("tevent_add_signal failed.\n"));
        ret = ENOMEM;
        goto fail;
    }


    *ops = &krb5_auth_ops;
    *pvt_auth_data = ctx;
    return EOK;

fail:
    talloc_free(ctx);
    return ret;
}

int sssm_krb5_chpass_init(struct be_ctx *bectx, struct bet_ops **ops,
                   void **pvt_auth_data)
{
    return sssm_krb5_auth_init(bectx, ops, pvt_auth_data);
}
