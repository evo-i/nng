//
// Copyright 2016 Garrett D'Amore <garrett@damore.org>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"

// This file contains functions relating to pipes.
//
// Operations on pipes (to the transport) are generally blocking operations,
// performed in the context of the protocol.

// nni_pipe_id returns the 32-bit pipe id, which can be used in backtraces.
uint32_t
nni_pipe_id(nni_pipe *p)
{
	return (p->p_id);
}


int
nni_pipe_send(nni_pipe *p, nng_msg *msg)
{
	return (p->p_ops.p_send(p->p_trandata, msg));
}


int
nni_pipe_recv(nni_pipe *p, nng_msg **msgp)
{
	return (p->p_ops.p_recv(p->p_trandata, msgp));
}


// nni_pipe_close closes the underlying connection.  It is expected that
// subsequent attempts receive or send (including any waiting receive) will
// simply return NNG_ECLOSED.
void
nni_pipe_close(nni_pipe *p)
{
	nni_socket *sock = p->p_sock;

	p->p_ops.p_close(p->p_trandata);

	nni_mutex_enter(&sock->s_mx);
	if (!p->p_reap) {
		// schedule deferred reap/close
		p->p_reap = 1;
		if (p->p_active) {
			nni_list_remove(&sock->s_pipes, p);
			p->p_active = 0;
		}
		nni_list_append(&sock->s_reaps, p);
		nni_cond_broadcast(&sock->s_cv);
	}
	nni_mutex_exit(&sock->s_mx);
}


uint16_t
nni_pipe_peer(nni_pipe *p)
{
	return (p->p_ops.p_peer(p->p_trandata));
}


void
nni_pipe_destroy(nni_pipe *p)
{
	if (p->p_send_thr != NULL) {
		nni_thread_reap(p->p_send_thr);
	}
	if (p->p_recv_thr != NULL) {
		nni_thread_reap(p->p_recv_thr);
	}
	if (p->p_trandata != NULL) {
		p->p_ops.p_destroy(p->p_trandata);
	}
	nni_cond_fini(&p->p_cv);
	if (p->p_pdata != NULL) {
		nni_free(p->p_pdata, p->p_psize);
	}
	nni_free(p, sizeof (*p));
}


int
nni_pipe_create(nni_pipe **pp, nni_endpt *ep)
{
	nni_pipe *p;
	nni_socket *sock = ep->ep_sock;
	int rv;

	if ((p = nni_alloc(sizeof (*p))) == NULL) {
		return (NNG_ENOMEM);
	}
	p->p_send_thr = NULL;
	p->p_recv_thr = NULL;
	p->p_trandata = NULL;
	p->p_active = 0;
	p->p_abort = 0;
	if ((rv = nni_cond_init(&p->p_cv, &sock->s_mx)) != 0) {
		nni_free(p, sizeof (*p));
		return (rv);
	}
	p->p_psize = sock->s_ops.proto_pipe_size;
	if ((p->p_pdata = nni_alloc(p->p_psize)) == NULL) {
		nni_cond_fini(&p->p_cv);
		nni_free(p, sizeof (*p));
		return (NNG_ENOMEM);
	}
	p->p_sock = sock;
	p->p_ops = *ep->ep_ops.ep_pipe_ops;
	NNI_LIST_NODE_INIT(&p->p_node);
	*pp = p;
	return (0);
}


int
nni_pipe_getopt(nni_pipe *p, int opt, void *val, size_t *szp)
{
	/*  This should only be called with the mutex held... */
	if (p->p_ops.p_getopt == NULL) {
		return (NNG_ENOTSUP);
	}
	return (p->p_ops.p_getopt(p->p_trandata, opt, val, szp));
}


static void
nni_pipe_sender(void *arg)
{
	nni_pipe *p = arg;

	nni_mutex_enter(&p->p_sock->s_mx);
	while ((!p->p_active) && (!p->p_abort)) {
		nni_cond_wait(&p->p_cv);
	}
	if (p->p_abort) {
		nni_mutex_exit(&p->p_sock->s_mx);
		return;
	}
	nni_mutex_exit(&p->p_sock->s_mx);
	if (p->p_sock->s_ops.proto_pipe_send != NULL) {
		p->p_sock->s_ops.proto_pipe_send(p->p_pdata);
	}
}


static void
nni_pipe_receiver(void *arg)
{
	nni_pipe *p = arg;

	nni_mutex_enter(&p->p_sock->s_mx);
	while ((!p->p_active) && (!p->p_abort)) {
		nni_cond_wait(&p->p_cv);
	}
	if (p->p_abort) {
		nni_mutex_exit(&p->p_sock->s_mx);
		return;
	}
	nni_mutex_exit(&p->p_sock->s_mx);
	if (p->p_sock->s_ops.proto_pipe_recv != NULL) {
		p->p_sock->s_ops.proto_pipe_recv(p->p_pdata);
	}
}


int
nni_pipe_start(nni_pipe *pipe)
{
	int rv;
	int collide;
	nni_socket *sock = pipe->p_sock;

	nni_mutex_enter(&sock->s_mx);
	if (sock->s_closing) {
		nni_mutex_exit(&sock->s_mx);
		return (NNG_ECLOSED);
	}

	do {
		// We generate a new pipe ID, but we make sure it does not
		// collide with any we already have.  This can only normally
		// happen if we wrap -- i.e. we've had 4 billion or so pipes.
		// XXX: consider making this a hash table!!
		nni_pipe *check;
		pipe->p_id = nni_plat_nextid() & 0x7FFFFFFF;
		collide = 0;
		NNI_LIST_FOREACH (&sock->s_pipes, check) {
			if (check->p_id == pipe->p_id) {
				collide = 1;
				break;
			}
		}
	} while (collide);

	rv = nni_thread_create(&pipe->p_send_thr, nni_pipe_sender, pipe);
	if (rv != 0) {
		goto fail;
	}
	rv = nni_thread_create(&pipe->p_recv_thr, nni_pipe_receiver, pipe);
	if (rv != 0) {
		goto fail;
	}

	rv = sock->s_ops.proto_add_pipe(sock->s_data, pipe, pipe->p_pdata);
	if (rv != 0) {
		goto fail;
	}
	nni_list_append(&sock->s_pipes, pipe);
	pipe->p_active = 1;

	// XXX: Publish event
	nni_cond_broadcast(&pipe->p_cv);

	nni_mutex_exit(&sock->s_mx);
	return (0);

fail:
	pipe->p_abort = 1;
	pipe->p_reap = 1;
	nni_list_append(&sock->s_reaps, pipe);
	nni_cond_broadcast(&sock->s_cv);
	nni_cond_broadcast(&pipe->p_cv);
	nni_mutex_exit(&sock->s_mx);
	return (rv);
}
