//
// Copyright 2017 Garrett D'Amore <garrett@damore.org>
// Copyright 2017 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include "core/nng_impl.h"

#ifdef PLATFORM_POSIX_EPDESC
#include "platform/posix/posix_aio.h"
#include "platform/posix/posix_pollq.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef SOCK_CLOEXEC
#define NNI_STREAM_SOCKTYPE (SOCK_STREAM | SOCK_CLOEXEC)
#else
#define NNI_STREAM_SOCKTYPE SOCK_STREAM
#endif

struct nni_posix_epdesc {
	nni_posix_pollq_node    node;
	nni_list                connectq;
	nni_list                acceptq;
	int                     closed;
	struct sockaddr_storage locaddr;
	struct sockaddr_storage remaddr;
	socklen_t               loclen;
	socklen_t               remlen;
	const char *            url;
	nni_mtx                 mtx;
};

static void
nni_posix_epdesc_cancel(nni_aio *aio, int rv)
{
	nni_posix_epdesc *ed = aio->a_prov_data;

	NNI_ASSERT(rv != 0);
	nni_mtx_lock(&ed->mtx);
	if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
		NNI_ASSERT(aio->a_pipe == NULL);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&ed->mtx);
}

static void
nni_posix_epdesc_finish(nni_aio *aio, int rv, int newfd)
{
	nni_posix_pipedesc *pd = NULL;

	// acceptq or connectq.
	nni_aio_list_remove(aio);

	if (rv == 0) {
		if ((rv = nni_posix_pipedesc_init(&pd, newfd)) != 0) {
			(void) close(newfd);
		}
	}
	if (rv != 0) {
		nni_aio_finish_error(aio, rv);
	} else {
		nni_aio_finish_pipe(aio, pd);
	}
}

static void
nni_posix_epdesc_doconnect(nni_posix_epdesc *ed)
{
	nni_aio * aio;
	socklen_t sz;
	int       rv;

	// Note that normally there will only be a single connect AIO...
	// A socket that is here will have *initiated* with a connect()
	// call, which returned EINPROGRESS.  When the connection attempt
	// is done, either way, the descriptor will be noted as writable.
	// getsockopt() with SOL_SOCKET, SO_ERROR to determine the actual
	// status of the connection attempt...
	while ((aio = nni_list_first(&ed->connectq)) != NULL) {
		rv = -1;
		sz = sizeof(rv);
		if (getsockopt(ed->node.fd, SOL_SOCKET, SO_ERROR, &rv, &sz) <
		    0) {
			rv = errno;
		}
		switch (rv) {
		case 0:
			// Success!
			nni_posix_epdesc_finish(aio, 0, ed->node.fd);
			ed->node.fd = -1;
			continue;

		case EINPROGRESS:
			// Still in progress... keep trying
			return;

		default:
			if (rv == ENOENT) {
				rv = ECONNREFUSED;
			}
			nni_posix_epdesc_finish(aio, nni_plat_errno(rv), 0);
			(void) close(ed->node.fd);
			ed->node.fd = -1;
			continue;
		}
	}
}

static void
nni_posix_epdesc_doaccept(nni_posix_epdesc *ed)
{
	nni_aio *aio;
	int      newfd;

	while ((aio = nni_list_first(&ed->acceptq)) != NULL) {
// We could argue that knowing the remote peer address would
// be nice.  But frankly if someone wants it, they can just
// do getpeername().

#ifdef NNG_USE_ACCEPT4
		newfd = accept4(ed->node.fd, NULL, NULL, SOCK_CLOEXEC);
		if ((newfd < 0) && ((errno == ENOSYS) || (errno == ENOTSUP))) {
			newfd = accept(ed->node.fd, NULL, NULL);
		}
#else
		newfd = accept(ed->node.fd, NULL, NULL);
#endif

		if (newfd >= 0) {
			// successful connection request!
			nni_posix_epdesc_finish(aio, 0, newfd);
			continue;
		}

		if ((errno == EWOULDBLOCK) || (errno == EAGAIN)) {
			// Well, let's try later.  Note that EWOULDBLOCK
			// is required by standards, but some platforms may
			// use EAGAIN.  The values may be the same, so we
			// can't use switch.
			return;
		}

		if ((errno == ECONNABORTED) || (errno == ECONNRESET)) {
			// Let's just eat this one.  Perhaps it may be
			// better to report it to the application, but we
			// think most applications don't want to see this.
			// Only someone with a packet trace is going to
			// notice this.
			continue;
		}

		nni_posix_epdesc_finish(aio, nni_plat_errno(errno), 0);
	}
}

static void
nni_posix_epdesc_doerror(nni_posix_epdesc *ed)
{
	nni_aio * aio;
	int       rv = 1;
	socklen_t sz = sizeof(rv);

	if (getsockopt(ed->node.fd, SOL_SOCKET, SO_ERROR, &rv, &sz) < 0) {
		rv = errno;
	}
	if (rv == 0) {
		return;
	}
	rv = nni_plat_errno(rv);

	while ((aio = nni_list_first(&ed->acceptq)) != NULL) {
		nni_posix_epdesc_finish(aio, rv, 0);
	}
	while ((aio = nni_list_first(&ed->connectq)) != NULL) {
		nni_posix_epdesc_finish(aio, rv, 0);
	}
}

static void
nni_posix_epdesc_doclose(nni_posix_epdesc *ed)
{
	nni_aio *           aio;
	struct sockaddr_un *sun;
	int                 fd;

	ed->closed = 1;
	while ((aio = nni_list_first(&ed->acceptq)) != NULL) {
		nni_posix_epdesc_finish(aio, NNG_ECLOSED, 0);
	}
	while ((aio = nni_list_first(&ed->connectq)) != NULL) {
		nni_posix_epdesc_finish(aio, NNG_ECLOSED, 0);
	}

	if ((fd = ed->node.fd) != -1) {
		ed->node.fd = -1;
		(void) shutdown(fd, SHUT_RDWR);
		(void) close(fd);
		sun = (void *) &ed->locaddr;
		if ((sun->sun_family == AF_UNIX) && (ed->loclen != 0)) {
			(void) unlink(sun->sun_path);
		}
	}
}

static void
nni_posix_epdesc_cb(void *arg)
{
	nni_posix_epdesc *ed = arg;
	int               events;

	nni_mtx_lock(&ed->mtx);

	if (ed->node.revents & POLLIN) {
		nni_posix_epdesc_doaccept(ed);
	}
	if (ed->node.revents & POLLOUT) {
		nni_posix_epdesc_doconnect(ed);
	}
	if (ed->node.revents & (POLLERR | POLLHUP)) {
		nni_posix_epdesc_doerror(ed);
	}
	if (ed->node.revents & POLLNVAL) {
		nni_posix_epdesc_doclose(ed);
	}

	events = 0;
	if (!nni_list_empty(&ed->connectq)) {
		events |= POLLOUT;
	}
	if (!nni_list_empty(&ed->acceptq)) {
		events |= POLLIN;
	}
	nni_posix_pollq_arm(&ed->node, events);
	nni_mtx_unlock(&ed->mtx);
}

void
nni_posix_epdesc_close(nni_posix_epdesc *ed)
{
	nni_posix_pollq_disarm(&ed->node, POLLIN | POLLOUT);

	nni_mtx_lock(&ed->mtx);
	nni_posix_epdesc_doclose(ed);
	nni_mtx_unlock(&ed->mtx);
}

static int
nni_posix_epdesc_parseaddr(char *pair, char **hostp, uint16_t *portp)
{
	char *host, *port, *end;
	char  c;
	int   val;

	if (pair[0] == '[') {
		host = pair + 1;
		// IP address enclosed ... for IPv6 usually.
		if ((end = strchr(host, ']')) == NULL) {
			return (NNG_EADDRINVAL);
		}
		*end = '\0';
		port = end + 1;
		if (*port == ':') {
			port++;
		} else if (*port != '\0') {
			return (NNG_EADDRINVAL);
		}
	} else {
		host = pair;
		port = strchr(host, ':');
		if (port != NULL) {
			*port = '\0';
			port++;
		}
	}
	val = 0;
	while ((c = *port) != '\0') {
		val *= 10;
		if ((c >= '0') && (c <= '9')) {
			val += (c - '0');
		} else {
			return (NNG_EADDRINVAL);
		}
		if (val > 65535) {
			return (NNG_EADDRINVAL);
		}
		port++;
	}
	if ((strlen(host) == 0) || (strcmp(host, "*") == 0)) {
		*hostp = NULL;
	} else {
		*hostp = host;
	}
	// Stash the port in big endian (network) byte order.
	NNI_PUT16((uint8_t *) portp, val);
	return (0);
}

int
nni_posix_epdesc_listen(nni_posix_epdesc *ed)
{
	int                      len;
	struct sockaddr_storage *ss;
	int                      rv;
	int                      fd;

	nni_mtx_lock(&ed->mtx);

	ss  = &ed->locaddr;
	len = ed->loclen;

	if ((fd = socket(ss->ss_family, NNI_STREAM_SOCKTYPE, 0)) < 0) {
		return (nni_plat_errno(errno));
	}
	(void) fcntl(fd, F_SETFD, FD_CLOEXEC);

	if (bind(fd, (struct sockaddr *) ss, len) < 0) {
		nni_mtx_unlock(&ed->mtx);
		rv = nni_plat_errno(errno);
		(void) close(fd);
		return (rv);
	}

	// Listen -- 128 depth is probably sufficient.  If it isn't, other
	// bad things are going to happen.
	if (listen(fd, 128) != 0) {
		nni_mtx_unlock(&ed->mtx);
		rv = nni_plat_errno(errno);
		(void) close(fd);
		return (rv);
	}

	(void) fcntl(fd, F_SETFL, O_NONBLOCK);

	ed->node.fd = fd;
	nni_mtx_unlock(&ed->mtx);
	return (0);
}

void
nni_posix_epdesc_accept(nni_posix_epdesc *ed, nni_aio *aio)
{
	int rv;

	// Accept is simpler than the connect case.  With accept we just
	// need to wait for the socket to be readable to indicate an incoming
	// connection is ready for us.  There isn't anything else for us to
	// do really, as that will have been done in listen.
	nni_mtx_lock(&ed->mtx);
	// If we can't start, it means that the AIO was stopped.
	if ((rv = nni_aio_start(aio, nni_posix_epdesc_cancel, ed)) != 0) {
		nni_mtx_unlock(&ed->mtx);
		return;
	}

	if (ed->closed) {
		nni_posix_epdesc_finish(aio, NNG_ECLOSED, 0);
		nni_mtx_unlock(&ed->mtx);
		return;
	}

	nni_aio_list_append(&ed->acceptq, aio);
	nni_posix_pollq_arm(&ed->node, POLLIN);
	nni_mtx_unlock(&ed->mtx);
}

void
nni_posix_epdesc_connect(nni_posix_epdesc *ed, nni_aio *aio)
{
	// NB: We assume that the FD is already set to nonblocking mode.
	int rv;
	int fd;

	nni_mtx_lock(&ed->mtx);
	// If we can't start, it means that the AIO was stopped.
	if ((rv = nni_aio_start(aio, nni_posix_epdesc_cancel, ed)) != 0) {
		nni_mtx_unlock(&ed->mtx);
		return;
	}

	fd = socket(ed->remaddr.ss_family, NNI_STREAM_SOCKTYPE, 0);
	if (fd < 0) {
		nni_posix_epdesc_finish(aio, rv, 0);
		nni_mtx_unlock(&ed->mtx);
		return;
	}

	// Possibly bind.
	if (ed->loclen != 0) {
		rv = bind(fd, (void *) &ed->locaddr, ed->loclen);
		if (rv != 0) {
			(void) close(fd);
			nni_posix_epdesc_finish(aio, rv, 0);
			nni_mtx_unlock(&ed->mtx);
			return;
		}
	}

	(void) fcntl(fd, F_SETFL, O_NONBLOCK);

	rv = connect(fd, (void *) &ed->remaddr, ed->remlen);

	if (rv == 0) {
		// Immediate connect, cool!  This probably only happens on
		// loopback, and probably not on every platform.

		nni_posix_epdesc_finish(aio, 0, fd);
		nni_mtx_unlock(&ed->mtx);
		return;
	}

	if (errno != EINPROGRESS) {
		// Some immediate failure occurred.
		if (errno == ENOENT) {
			errno = ECONNREFUSED;
		}
		(void) close(fd);
		nni_posix_epdesc_finish(aio, nni_plat_errno(errno), 0);
		nni_mtx_unlock(&ed->mtx);
		return;
	}

	// We have to submit to the pollq, because the connection is pending.
	ed->node.fd = fd;
	nni_aio_list_append(&ed->connectq, aio);
	nni_posix_pollq_arm(&ed->node, POLLOUT);
	nni_mtx_unlock(&ed->mtx);
}

int
nni_posix_epdesc_init(nni_posix_epdesc **edp, const char *url)
{
	nni_posix_epdesc *ed;
	nni_posix_pollq * pq;
	int               rv;

	if ((ed = NNI_ALLOC_STRUCT(ed)) == NULL) {
		return (NNG_ENOMEM);
	}

	nni_mtx_init(&ed->mtx);

	// We could randomly choose a different pollq, or for efficiencies
	// sake we could take a modulo of the file desc number to choose
	// one.  For now we just have a global pollq.  Note that by tying
	// the ed to a single pollq we may get some kind of cache warmth.

	ed->node.index = 0;
	ed->node.cb    = nni_posix_epdesc_cb;
	ed->node.data  = ed;
	ed->url        = url;

	nni_aio_list_init(&ed->connectq);
	nni_aio_list_init(&ed->acceptq);

	pq = nni_posix_pollq_get(nni_random() % 0xffff);
	if ((rv = nni_posix_pollq_add(pq, &ed->node)) != 0) {
		nni_mtx_fini(&ed->mtx);
		NNI_FREE_STRUCT(ed);
		return (rv);
	}
	*edp = ed;
	return (0);
}

const char *
nni_posix_epdesc_url(nni_posix_epdesc *ed)
{
	return (ed->url);
}

void
nni_posix_epdesc_set_local(nni_posix_epdesc *ed, void *sa, int len)
{
	if ((len < 0) || (len > sizeof(struct sockaddr_storage))) {
		return;
	}
	nni_mtx_lock(&ed->mtx);
	memcpy(&ed->locaddr, sa, len);
	ed->loclen = len;
	nni_mtx_unlock(&ed->mtx);
}

void
nni_posix_epdesc_set_remote(nni_posix_epdesc *ed, void *sa, int len)
{
	if ((len < 0) || (len > sizeof(struct sockaddr_storage))) {
		return;
	}
	nni_mtx_lock(&ed->mtx);
	memcpy(&ed->remaddr, sa, len);
	ed->remlen = len;
	nni_mtx_unlock(&ed->mtx);
}

void
nni_posix_epdesc_fini(nni_posix_epdesc *ed)
{
	int fd;
	nni_mtx_lock(&ed->mtx);
	if ((fd = ed->node.fd) != -1) {
		(void) close(ed->node.fd);
		nni_posix_epdesc_doclose(ed);
	}
	nni_mtx_unlock(&ed->mtx);
	nni_posix_pollq_remove(&ed->node);
	nni_mtx_fini(&ed->mtx);
	NNI_FREE_STRUCT(ed);
}

#else

// Suppress empty symbols warnings in ranlib.
int nni_posix_epdesc_not_used = 0;

#endif // PLATFORM_POSIX_EPDESC
