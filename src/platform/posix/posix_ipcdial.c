//
// Copyright 2024 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
// Copyright 2019 Devolutions <info@devolutions.net>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

#include "posix_ipc.h"

typedef struct nni_ipc_dialer ipc_dialer;

// Dialer stuff.
static void
ipc_dialer_close(void *arg)
{
	ipc_dialer *d = arg;
	nni_mtx_lock(&d->mtx);
	if (!d->closed) {
		nni_aio *aio;
		d->closed = true;
		while ((aio = nni_list_first(&d->connq)) != NULL) {
			nni_ipc_conn *c;
			nni_list_remove(&d->connq, aio);
			if ((c = nni_aio_get_prov_data(aio)) != NULL) {
				c->dial_aio = NULL;
				nni_aio_set_prov_data(aio, NULL);
				nng_stream_close(&c->stream);
				nng_stream_stop(&c->stream);
				nng_stream_free(&c->stream);
			}
			nni_aio_finish_error(aio, NNG_ECLOSED);
		}
	}
	nni_mtx_unlock(&d->mtx);
}

static void
ipc_dialer_stop(void *arg)
{
	nni_ipc_dialer *d = arg;
	ipc_dialer_close(d);
}

static void
ipc_dialer_fini(void *arg)
{
	ipc_dialer *d = arg;
	nni_mtx_fini(&d->mtx);
	NNI_FREE_STRUCT(d);
}

static void
ipc_dialer_free(void *arg)
{
	ipc_dialer *d = arg;

	ipc_dialer_stop(d);

	nni_posix_ipc_dialer_rele(d);
}

void
nni_posix_ipc_dialer_rele(ipc_dialer *d)
{
	nni_refcnt_rele(&d->ref);
}

static void
ipc_dialer_cancel(nni_aio *aio, void *arg, nng_err rv)
{
	nni_ipc_dialer *d = arg;
	nni_ipc_conn   *c;

	nni_mtx_lock(&d->mtx);
	if ((!nni_aio_list_active(aio)) ||
	    ((c = nni_aio_get_prov_data(aio)) == NULL)) {
		nni_mtx_unlock(&d->mtx);
		return;
	}
	nni_aio_list_remove(aio);
	c->dial_aio = NULL;
	nni_aio_set_prov_data(aio, NULL);
	nni_mtx_unlock(&d->mtx);

	nni_aio_finish_error(aio, rv);
	nng_stream_stop(&c->stream);
	nng_stream_free(&c->stream);
}

void
nni_posix_ipc_dialer_cb(void *arg, unsigned ev)
{
	nni_ipc_conn   *c = arg;
	nni_ipc_dialer *d = c->dialer;
	nni_aio        *aio;
	int             rv;

	nni_mtx_lock(&d->mtx);
	aio = c->dial_aio;
	if ((aio == NULL) || (!nni_aio_list_active(aio))) {
		nni_mtx_unlock(&d->mtx);
		return;
	}

	if ((ev & NNI_POLL_INVAL) != 0) {
		rv = EBADF;

	} else {
		socklen_t sz = sizeof(int);
		int       fd = nni_posix_pfd_fd(&c->pfd);
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &rv, &sz) < 0) {
			rv = errno;
		}
		if (rv == EINPROGRESS) {
			// Connection still in progress, come back
			// later.
			nni_mtx_unlock(&d->mtx);
			return;
		} else if (rv != 0) {
			rv = nni_plat_errno(rv);
		}
	}

	c->dial_aio = NULL;
	nni_aio_list_remove(aio);
	nni_aio_set_prov_data(aio, NULL);
	nni_mtx_unlock(&d->mtx);

	if (rv != 0) {
		nng_stream_close(&c->stream);
		nng_stream_stop(&c->stream);
		nng_stream_free(&c->stream);
		nni_aio_finish_error(aio, rv);
		return;
	}

	nni_aio_set_output(aio, 0, c);
	nni_aio_finish(aio, 0, 0);
}

// We don't give local address binding support.  Outbound dialers always
// get an ephemeral port.
void
ipc_dialer_dial(void *arg, nni_aio *aio)
{
	ipc_dialer             *d = arg;
	nni_ipc_conn           *c;
	struct sockaddr_storage ss;
	size_t                  len;
	int                     fd;
	int                     rv;

	nni_aio_reset(aio);

	if (((len = nni_posix_nn2sockaddr(&ss, &d->sa)) == 0) ||
	    (ss.ss_family != AF_UNIX)) {
		nni_aio_finish_error(aio, NNG_EADDRINVAL);
		return;
	}

	if ((fd = socket(ss.ss_family, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0) {
		nni_aio_finish_error(aio, nni_plat_errno(errno));
		return;
	}

	if ((rv = nni_posix_ipc_alloc(&c, &d->sa, d, fd)) != 0) {
		(void) close(fd);
		nni_aio_finish_error(aio, rv);
		return;
	}

	// hold for the conn
	nni_refcnt_hold(&d->ref);

	nni_mtx_lock(&d->mtx);
	if (!nni_aio_start(aio, ipc_dialer_cancel, d)) {
		nni_mtx_unlock(&d->mtx);
		nng_stream_free(&c->stream);
		return;
	}

	if (d->closed) {
		rv = NNG_ECLOSED;
		goto error;
	}
	c->dial_aio = aio;
#ifdef NNG_TEST_LIB
	// this stanza exists for testing, because IPC doesn't normally
	// exhibit a delayed connection.  But it could, and we need to try
	// to test it as much as possible.
	if (d->no_connect) {
		errno = EINPROGRESS;
	}
	if (d->no_connect || (connect(fd, (void *) &ss, len) != 0))
#else
	if (connect(fd, (void *) &ss, len) != 0)
#endif

	{
		if (errno != EINPROGRESS && errno != EAGAIN) {
			if (errno == ENOENT) {
				// No socket present means nobody listening.
				rv = NNG_ECONNREFUSED;
			} else {
				rv = nni_plat_errno(errno);
			}
			goto error;
		}
		// Asynchronous connect.
		if ((rv = nni_posix_pfd_arm(&c->pfd, NNI_POLL_OUT)) != 0) {
			goto error;
		}
		c->dial_aio = NULL;
		nni_aio_set_prov_data(aio, c);
		nni_list_append(&d->connq, aio);
		nni_mtx_unlock(&d->mtx);
		return;
	}
	// Immediate connect, cool! For IPC this is typical.
	c->dial_aio = NULL;
	nni_aio_set_prov_data(aio, NULL);
	nni_mtx_unlock(&d->mtx);
	nni_aio_set_output(aio, 0, c);
	nni_aio_finish(aio, 0, 0);
	return;

error:
	c->dial_aio = NULL;
	nni_aio_set_prov_data(aio, NULL);
	nni_mtx_unlock(&d->mtx);
	nng_stream_stop(&c->stream);
	nng_stream_free(&c->stream);
	nni_aio_finish_error(aio, rv);
}

static nng_err
ipc_dialer_get_remaddr(void *arg, void *buf, size_t *szp, nni_type t)
{
	ipc_dialer *d = arg;

	return (nni_copyout_sockaddr(&d->sa, buf, szp, t));
}

#ifdef NNG_TEST_LIB
static nng_err
ipc_dialer_set_test_no_connect(
    void *arg, const void *buf, size_t sz, nni_type t)
{
	ipc_dialer *d          = arg;
	bool        no_connect = false;

	(void) nni_copyin_bool(&no_connect, buf, sz, t);
	nni_mtx_lock(&d->mtx);
	d->no_connect = no_connect;
	nni_mtx_unlock(&d->mtx);
	return (0);
}
#endif

static const nni_option ipc_dialer_options[] = {
	{
	    .o_name = NNG_OPT_REMADDR,
	    .o_get  = ipc_dialer_get_remaddr,
	},
#ifdef NNG_TEST_LIB
	{
	    .o_name = "test-no-connect",
	    .o_set  = ipc_dialer_set_test_no_connect,
	},
#endif
	{
	    .o_name = NULL,
	},
};

static nng_err
ipc_dialer_get(void *arg, const char *nm, void *buf, size_t *szp, nni_type t)
{
	ipc_dialer *d = arg;
	return (nni_getopt(ipc_dialer_options, nm, d, buf, szp, t));
}

static nng_err
ipc_dialer_set(
    void *arg, const char *nm, const void *buf, size_t sz, nni_type t)
{
	ipc_dialer *d = arg;
	return (nni_setopt(ipc_dialer_options, nm, d, buf, sz, t));
}

nng_err
nni_ipc_dialer_alloc(nng_stream_dialer **dp, const nng_url *url)
{
	ipc_dialer *d;
	size_t      len;

	if ((d = NNI_ALLOC_STRUCT(d)) == NULL) {
		return (NNG_ENOMEM);
	}

	if ((strcmp(url->u_scheme, "ipc") == 0) ||
	    (strcmp(url->u_scheme, "unix") == 0)) {
		if ((url->u_path == NULL) ||
		    ((len = strlen(url->u_path)) == 0) ||
		    (len > NNG_MAXADDRLEN)) {
			NNI_FREE_STRUCT(d);
			return (NNG_EADDRINVAL);
		}
		d->sa.s_ipc.sa_family = NNG_AF_IPC;
		nni_strlcpy(d->sa.s_ipc.sa_path, url->u_path, NNG_MAXADDRLEN);

#ifdef NNG_HAVE_ABSTRACT_SOCKETS
	} else if (strcmp(url->u_scheme, "abstract") == 0) {

		// path is url encoded.
		len = nni_url_decode(d->sa.s_abstract.sa_name, url->u_path,
		    sizeof(d->sa.s_abstract.sa_name));
		if (len == (size_t) -1) {
			NNI_FREE_STRUCT(d);
			return (NNG_EADDRINVAL);
		}

		d->sa.s_abstract.sa_family = NNG_AF_ABSTRACT;
		d->sa.s_abstract.sa_len    = len;
#endif

	} else {
		NNI_FREE_STRUCT(d);
		return (NNG_EADDRINVAL);
	}

	nni_mtx_init(&d->mtx);
	nni_aio_list_init(&d->connq);
	d->closed      = false;
	d->sd.sd_free  = ipc_dialer_free;
	d->sd.sd_close = ipc_dialer_close;
	d->sd.sd_stop  = ipc_dialer_stop;
	d->sd.sd_dial  = ipc_dialer_dial;
	d->sd.sd_get   = ipc_dialer_get;
	d->sd.sd_set   = ipc_dialer_set;
	nni_refcnt_init(&d->ref, 1, d, ipc_dialer_fini);

	*dp = (void *) d;
	return (0);
}
