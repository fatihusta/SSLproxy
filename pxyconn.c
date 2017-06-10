/*
 * SSLsplit - transparent SSL/TLS interception
 * Copyright (c) 2009-2016, Daniel Roethlisberger <daniel@roe.ch>
 * All rights reserved.
 * http://www.roe.ch/SSLsplit
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "pxyconn.h"

#include "pxysslshut.h"
#include "cachemgr.h"
#include "ssl.h"
#include "opts.h"
#include "sys.h"
#include "util.h"
#include "base64.h"
#include "url.h"
#include "log.h"
#include "attrib.h"
#include "proc.h"

#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <event2/event.h>
#include <event2/listener.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>
#include <event2/buffer.h>
#include <event2/thread.h>
#include <event2/dns.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <assert.h>


/*
 * Maximum size of data to buffer per connection direction before
 * temporarily stopping to read data from the other end.
 */
#define OUTBUF_LIMIT	(128*1024)

/*
 * Print helper for logging code.
 */
#define STRORDASH(x)	(((x)&&*(x))?(x):"-")

/*
 * Context used for all server sessions.
 */
#ifdef USE_SSL_SESSION_ID_CONTEXT
static unsigned long ssl_session_context = 0x31415926;
#endif /* USE_SSL_SESSION_ID_CONTEXT */


/*
 * Proxy connection context state, describes a proxy connection
 * with source and destination socket bufferevents, SSL context and
 * other session state.  One of these exists per handled proxy
 * connection.
 */

#define WANT_CONNECT_LOG(ctx)	((ctx)->opts->connectlog||!(ctx)->opts->detach)
#define WANT_CONTENT_LOG(ctx)	((ctx)->opts->contentlog&&!(ctx)->passthrough)

static void
pxy_conn_connect_e2(pxy_conn_ctx_t *ctx);

static pxy_conn_ctx_t *
pxy_conn_ctx_new(proxyspec_t *spec, opts_t *opts,
                 pxy_thrmgr_ctx_t *thrmgr, evutil_socket_t fd)
{
	log_dbg_printf(">>>>>................... pxy_conn_ctx_new: ENTER fd=%d, sizeof(pxy_conn_ctx_t)=%d\n", fd, sizeof(pxy_conn_ctx_t));

	pxy_conn_ctx_t *ctx = malloc(sizeof(pxy_conn_ctx_t));
	if (!ctx)
		return NULL;
	memset(ctx, 0, sizeof(pxy_conn_ctx_t));
	ctx->spec = spec;
	ctx->opts = opts;
	ctx->clienthello_search = spec->upgrade;
	ctx->fd = fd;
	ctx->thridx = pxy_thrmgr_attach(thrmgr, &ctx->evbase, &ctx->dnsbase);
	ctx->thrmgr = thrmgr;
#ifdef HAVE_LOCAL_PROCINFO
	ctx->lproc.pid = -1;
#endif /* HAVE_LOCAL_PROCINFO */
#ifdef DEBUG_PROXY
	if (OPTS_DEBUG(opts)) {
		log_dbg_printf("%p             pxy_conn_ctx_new\n",
		               (void*)ctx);
	}
#endif /* DEBUG_PROXY */
	log_dbg_printf(">>>>>................... pxy_conn_ctx_new: EXIT fd=%d\n", fd);
	return ctx;
}

static pxy_conn_ctx_t *
pxy_conn_ctx_reinit(pxy_conn_ctx_t *ctx, proxyspec_t *spec, opts_t *opts,
                 pxy_thrmgr_ctx_t *thrmgr, evutil_socket_t fd)
{
	log_dbg_printf(">>>>>................... pxy_conn_ctx_reinit: ENTER fd=%d\n", fd);

	if (!ctx)
		return NULL;

	ctx->spec = spec;
	ctx->opts = opts;
	ctx->clienthello_search = spec->upgrade;
	ctx->fd = fd;
	ctx->thridx = pxy_thrmgr_attach(thrmgr, &ctx->evbase, &ctx->dnsbase);
	ctx->thrmgr = thrmgr;
#ifdef HAVE_LOCAL_PROCINFO
	ctx->lproc.pid = -1;
#endif /* HAVE_LOCAL_PROCINFO */
#ifdef DEBUG_PROXY
	if (OPTS_DEBUG(opts)) {
		log_dbg_printf("%p             pxy_conn_ctx_new\n",
		               (void*)ctx);
	}
#endif /* DEBUG_PROXY */
	log_dbg_printf(">>>>>................... pxy_conn_ctx_reinit: EXIT fd=%d\n", fd);
	return ctx;
}

static void NONNULL(1)
pxy_conn_ctx_free(pxy_conn_ctx_t *ctx)
{
#ifdef DEBUG_PROXY
	if (OPTS_DEBUG(ctx->opts)) {
		log_dbg_printf("%p             pxy_conn_ctx_free\n",
		                (void*)ctx);
	}
#endif /* DEBUG_PROXY */
	pxy_thrmgr_detach(ctx->thrmgr, ctx->thridx);
	if (ctx->srchost_str) {
		free(ctx->srchost_str);
	}
	if (ctx->srcport_str) {
		free(ctx->srcport_str);
	}
	if (ctx->dsthost_str) {
		free(ctx->dsthost_str);
	}
	if (ctx->dstport_str) {
		free(ctx->dstport_str);
	}
	if (ctx->http_method) {
		free(ctx->http_method);
	}
	if (ctx->http_uri) {
		free(ctx->http_uri);
	}
	if (ctx->http_host) {
		free(ctx->http_host);
	}
	if (ctx->http_content_type) {
		free(ctx->http_content_type);
	}
	if (ctx->http_status_code) {
		free(ctx->http_status_code);
	}
	if (ctx->http_status_text) {
		free(ctx->http_status_text);
	}
	if (ctx->http_content_length) {
		free(ctx->http_content_length);
	}
	if (ctx->ssl_names) {
		free(ctx->ssl_names);
	}
	if (ctx->origcrtfpr) {
		free(ctx->origcrtfpr);
	}
	if (ctx->usedcrtfpr) {
		free(ctx->usedcrtfpr);
	}
#ifdef HAVE_LOCAL_PROCINFO
	if (ctx->lproc.exec_path) {
		free(ctx->lproc.exec_path);
	}
	if (ctx->lproc.user) {
		free(ctx->lproc.user);
	}
	if (ctx->lproc.group) {
		free(ctx->lproc.group);
	}
#endif /* HAVE_LOCAL_PROCINFO */
	if (ctx->origcrt) {
		X509_free(ctx->origcrt);
	}
	if (ctx->ev) {
		event_free(ctx->ev);
	}
	if (ctx->sni) {
		free(ctx->sni);
	}
	if (WANT_CONTENT_LOG(ctx) && ctx->logctx) {
		if (log_content_close(&ctx->logctx) == -1) {
			log_err_printf("Warning: Content log close failed\n");
		}
	}
	free(ctx);
}


/* forward declaration of libevent callbacks */
static void pxy_bev_readcb(struct bufferevent *, void *);
static void pxy_bev_readcb_e2(struct bufferevent *, void *);
static void pxy_bev_writecb(struct bufferevent *, void *);
static void pxy_bev_writecb_e2(struct bufferevent *, void *);
static void pxy_bev_eventcb(struct bufferevent *, short, void *);
static void pxy_bev_eventcb_e2(struct bufferevent *, short, void *);
static void pxy_fd_readcb(evutil_socket_t, short, void *);

/* forward declaration of OpenSSL callbacks */
#ifndef OPENSSL_NO_TLSEXT
static int pxy_ossl_servername_cb(SSL *ssl, int *al, void *arg);
#endif /* !OPENSSL_NO_TLSEXT */
static int pxy_ossl_sessnew_cb(SSL *, SSL_SESSION *);
static void pxy_ossl_sessremove_cb(SSL_CTX *, SSL_SESSION *);
static SSL_SESSION * pxy_ossl_sessget_cb(SSL *, unsigned char *, int, int *);

/*
 * Dump information on a certificate to the debug log.
 */
static void
pxy_debug_crt(X509 *crt)
{
	char *sj = ssl_x509_subject(crt);
	if (sj) {
		log_dbg_printf("Subject DN: %s\n", sj);
		free(sj);
	}

	char *names = ssl_x509_names_to_str(crt);
	if (names) {
		log_dbg_printf("Common Names: %s\n", names);
		free(names);
	}

	char *fpr;
	if (!(fpr = ssl_x509_fingerprint(crt, 1))) {
		log_err_printf("Warning: Error generating X509 fingerprint\n");
	} else {
		log_dbg_printf("Fingerprint: %s\n", fpr);
		free(fpr);
	}

#ifdef DEBUG_CERTIFICATE
	/* dump certificate */
	log_dbg_print_free(ssl_x509_to_str(crt));
	log_dbg_print_free(ssl_x509_to_pem(crt));
#endif /* DEBUG_CERTIFICATE */
}

static void
pxy_log_connect_nonhttp(pxy_conn_ctx_t *ctx)
{
	char *msg;
#ifdef HAVE_LOCAL_PROCINFO
	char *lpi = NULL;
#endif /* HAVE_LOCAL_PROCINFO */
	int rv;

#ifdef HAVE_LOCAL_PROCINFO
	if (ctx->opts->lprocinfo) {
		rv = asprintf(&lpi, "lproc:%i:%s:%s:%s",
		              ctx->lproc.pid,
		              STRORDASH(ctx->lproc.user),
		              STRORDASH(ctx->lproc.group),
		              STRORDASH(ctx->lproc.exec_path));
		if ((rv < 0) || !lpi) {
			ctx->enomem = 1;
			goto out;
		}
	} else {
		lpi = "";
	}
#endif /* HAVE_LOCAL_PROCINFO */

	if (!ctx->src.ssl) {
		rv = asprintf(&msg, "%s %s %s %s %s"
#ifdef HAVE_LOCAL_PROCINFO
		              " %s"
#endif /* HAVE_LOCAL_PROCINFO */
		              "\n",
		              ctx->passthrough ? "passthrough" : "tcp",
		              STRORDASH(ctx->srchost_str),
		              STRORDASH(ctx->srcport_str),
		              STRORDASH(ctx->dsthost_str),
		              STRORDASH(ctx->dstport_str)
#ifdef HAVE_LOCAL_PROCINFO
		              , lpi
#endif /* HAVE_LOCAL_PROCINFO */
		             );
	} else {
		rv = asprintf(&msg, "%s %s %s %s %s "
		              "sni:%s names:%s "
		              "sproto:%s:%s dproto:%s:%s "
		              "origcrt:%s usedcrt:%s"
#ifdef HAVE_LOCAL_PROCINFO
		              " %s"
#endif /* HAVE_LOCAL_PROCINFO */
		              "\n",
		              ctx->clienthello_found ? "upgrade" : "ssl",
		              STRORDASH(ctx->srchost_str),
		              STRORDASH(ctx->srcport_str),
		              STRORDASH(ctx->dsthost_str),
		              STRORDASH(ctx->dstport_str),
		              STRORDASH(ctx->sni),
		              STRORDASH(ctx->ssl_names),
		              SSL_get_version(ctx->src.ssl),
		              SSL_get_cipher(ctx->src.ssl),
		              SSL_get_version(ctx->dst.ssl),
		              SSL_get_cipher(ctx->dst.ssl),
		              STRORDASH(ctx->origcrtfpr),
		              STRORDASH(ctx->usedcrtfpr)
#ifdef HAVE_LOCAL_PROCINFO
		              , lpi
#endif /* HAVE_LOCAL_PROCINFO */
		              );
	}
	if ((rv < 0) || !msg) {
		ctx->enomem = 1;
		goto out;
	}
	if (!ctx->opts->detach) {
		log_err_printf("%s", msg);
	}
	if (ctx->opts->connectlog) {
		if (log_connect_print_free(msg) == -1) {
			free(msg);
			log_err_printf("Warning: Connection logging failed\n");
		}
	} else {
		free(msg);
	}
out:
#ifdef HAVE_LOCAL_PROCINFO
	if (lpi && ctx->opts->lprocinfo) {
		free(lpi);
	}
#endif /* HAVE_LOCAL_PROCINFO */
	return;
}

static void
pxy_log_connect_http(pxy_conn_ctx_t *ctx)
{
	char *msg;
#ifdef HAVE_LOCAL_PROCINFO
	char *lpi = NULL;
#endif /* HAVE_LOCAL_PROCINFO */
	int rv;

#ifdef DEBUG_PROXY
	if (ctx->passthrough) {
		log_err_printf("Warning: pxy_log_connect_http called while in "
		               "passthrough mode\n");
		return;
	}
#endif

#ifdef HAVE_LOCAL_PROCINFO
	if (ctx->opts->lprocinfo) {
		rv = asprintf(&lpi, "lproc:%i:%s:%s:%s",
		              ctx->lproc.pid,
		              STRORDASH(ctx->lproc.user),
		              STRORDASH(ctx->lproc.group),
		              STRORDASH(ctx->lproc.exec_path));
		if ((rv < 0) || !lpi) {
			ctx->enomem = 1;
			goto out;
		}
	}
#endif /* HAVE_LOCAL_PROCINFO */

	if (!ctx->spec->ssl) {
		rv = asprintf(&msg, "http %s %s %s %s %s %s %s %s %s"
#ifdef HAVE_LOCAL_PROCINFO
		              " %s"
#endif /* HAVE_LOCAL_PROCINFO */
		              "%s\n",
		              STRORDASH(ctx->srchost_str),
		              STRORDASH(ctx->srcport_str),
		              STRORDASH(ctx->dsthost_str),
		              STRORDASH(ctx->dstport_str),
		              STRORDASH(ctx->http_host),
		              STRORDASH(ctx->http_method),
		              STRORDASH(ctx->http_uri),
		              STRORDASH(ctx->http_status_code),
		              STRORDASH(ctx->http_content_length),
#ifdef HAVE_LOCAL_PROCINFO
		              lpi,
#endif /* HAVE_LOCAL_PROCINFO */
		              ctx->ocsp_denied ? " ocsp:denied" : "");
	} else {
		rv = asprintf(&msg, "https %s %s %s %s %s %s %s %s %s "
		              "sni:%s names:%s "
		              "sproto:%s:%s dproto:%s:%s "
		              "origcrt:%s usedcrt:%s"
#ifdef HAVE_LOCAL_PROCINFO
		              " %s"
#endif /* HAVE_LOCAL_PROCINFO */
		              "%s\n",
		              STRORDASH(ctx->srchost_str),
		              STRORDASH(ctx->srcport_str),
		              STRORDASH(ctx->dsthost_str),
		              STRORDASH(ctx->dstport_str),
		              STRORDASH(ctx->http_host),
		              STRORDASH(ctx->http_method),
		              STRORDASH(ctx->http_uri),
		              STRORDASH(ctx->http_status_code),
		              STRORDASH(ctx->http_content_length),
		              STRORDASH(ctx->sni),
		              STRORDASH(ctx->ssl_names),
		              SSL_get_version(ctx->src.ssl),
		              SSL_get_cipher(ctx->src.ssl),
		              SSL_get_version(ctx->dst.ssl),
		              SSL_get_cipher(ctx->dst.ssl),
		              STRORDASH(ctx->origcrtfpr),
		              STRORDASH(ctx->usedcrtfpr),
#ifdef HAVE_LOCAL_PROCINFO
		              lpi,
#endif /* HAVE_LOCAL_PROCINFO */
		              ctx->ocsp_denied ? " ocsp:denied" : "");
	}
	if ((rv < 0 ) || !msg) {
		ctx->enomem = 1;
		goto out;
	}
	if (!ctx->opts->detach) {
		log_err_printf("%s", msg);
	}
	if (ctx->opts->connectlog) {
		if (log_connect_print_free(msg) == -1) {
			free(msg);
			log_err_printf("Warning: Connection logging failed\n");
		}
	} else {
		free(msg);
	}
out:
#ifdef HAVE_LOCAL_PROCINFO
	if (lpi) {
		free(lpi);
	}
#endif /* HAVE_LOCAL_PROCINFO */
	return;
}

/*
 * Called by OpenSSL when a new src SSL session is created.
 * OpenSSL increments the refcount before calling the callback and will
 * decrement it again if we return 0.  Returning 1 will make OpenSSL skip
 * the refcount decrementing.  In other words, return 0 if we did not
 * keep a pointer to the object (which we never do here).
 */
#ifdef HAVE_SSLV2
#define MAYBE_UNUSED 
#else /* !HAVE_SSLV2 */
#define MAYBE_UNUSED UNUSED
#endif /* !HAVE_SSLV2 */
static int
pxy_ossl_sessnew_cb(MAYBE_UNUSED SSL *ssl, SSL_SESSION *sess)
#undef MAYBE_UNUSED
{
#ifdef DEBUG_SESSION_CACHE
	log_dbg_printf("===> OpenSSL new session callback:\n");
	if (sess) {
		log_dbg_print_free(ssl_session_to_str(sess));
	} else {
		log_dbg_printf("(null)\n");
	}
#endif /* DEBUG_SESSION_CACHE */
#ifdef HAVE_SSLV2
	/* Session resumption seems to fail for SSLv2 with protocol
	 * parsing errors, so we disable caching for SSLv2. */
	if (SSL_version(ssl) == SSL2_VERSION) {
		log_err_printf("Warning: Session resumption denied to SSLv2"
		               "client.\n");
		return 0;
	}
#endif /* HAVE_SSLV2 */
	if (sess) {
		cachemgr_ssess_set(sess);
	}
	return 0;
}

/*
 * Called by OpenSSL when a src SSL session should be removed.
 * OpenSSL calls SSL_SESSION_free() after calling the callback;
 * we do not need to free the reference here.
 */
static void
pxy_ossl_sessremove_cb(UNUSED SSL_CTX *sslctx, SSL_SESSION *sess)
{
#ifdef DEBUG_SESSION_CACHE
	log_dbg_printf("===> OpenSSL remove session callback:\n");
	if (sess) {
		log_dbg_print_free(ssl_session_to_str(sess));
	} else {
		log_dbg_printf("(null)\n");
	}
#endif /* DEBUG_SESSION_CACHE */
	if (sess) {
		cachemgr_ssess_del(sess);
	}
}

/*
 * Called by OpenSSL when a src SSL session is requested by the client.
 */
static SSL_SESSION *
pxy_ossl_sessget_cb(UNUSED SSL *ssl, unsigned char *id, int idlen, int *copy)
{
	SSL_SESSION *sess;

#ifdef DEBUG_SESSION_CACHE
	log_dbg_printf("===> OpenSSL get session callback:\n");
#endif /* DEBUG_SESSION_CACHE */

	*copy = 0; /* SSL should not increment reference count of session */
	sess = cachemgr_ssess_get(id, idlen);

#ifdef DEBUG_SESSION_CACHE
	if (sess) {
		log_dbg_print_free(ssl_session_to_str(sess));
	}
#endif /* DEBUG_SESSION_CACHE */

	log_dbg_printf("SSL session cache: %s\n", sess ? "HIT" : "MISS");
	return sess;
}

/*
 * Set SSL_CTX options that are the same for incoming and outgoing SSL_CTX.
 */
static void
pxy_sslctx_setoptions(SSL_CTX *sslctx, pxy_conn_ctx_t *ctx)
{
	SSL_CTX_set_options(sslctx, SSL_OP_ALL);
#ifdef SSL_OP_TLS_ROLLBACK_BUG
	SSL_CTX_set_options(sslctx, SSL_OP_TLS_ROLLBACK_BUG);
#endif /* SSL_OP_TLS_ROLLBACK_BUG */
#ifdef SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION
	SSL_CTX_set_options(sslctx, SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION);
#endif /* SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION */
#ifdef SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS
	SSL_CTX_set_options(sslctx, SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS);
#endif /* SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS */
#ifdef SSL_OP_NO_TICKET
	SSL_CTX_set_options(sslctx, SSL_OP_NO_TICKET);
#endif /* SSL_OP_NO_TICKET */

	/*
	 * Do not use HAVE_SSLV2 because we need to set SSL_OP_NO_SSLv2 if it
	 * is available and WITH_SSLV2 was not used.
	 */
#ifdef SSL_OP_NO_SSLv2
#ifdef WITH_SSLV2
	if (ctx->opts->no_ssl2) {
#endif /* WITH_SSLV2 */
		SSL_CTX_set_options(sslctx, SSL_OP_NO_SSLv2);
#ifdef WITH_SSLV2
	}
#endif /* WITH_SSLV2 */
#endif /* !SSL_OP_NO_SSLv2 */
#ifdef HAVE_SSLV3
	if (ctx->opts->no_ssl3) {
		SSL_CTX_set_options(sslctx, SSL_OP_NO_SSLv3);
	}
#endif /* HAVE_SSLV3 */
#ifdef HAVE_TLSV10
	if (ctx->opts->no_tls10) {
		SSL_CTX_set_options(sslctx, SSL_OP_NO_TLSv1);
	}
#endif /* HAVE_TLSV10 */
#ifdef HAVE_TLSV11
	if (ctx->opts->no_tls11) {
		SSL_CTX_set_options(sslctx, SSL_OP_NO_TLSv1_1);
	}
#endif /* HAVE_TLSV11 */
#ifdef HAVE_TLSV12
	if (ctx->opts->no_tls12) {
		SSL_CTX_set_options(sslctx, SSL_OP_NO_TLSv1_2);
	}
#endif /* HAVE_TLSV12 */

#ifdef SSL_OP_NO_COMPRESSION
	if (!ctx->opts->sslcomp) {
		SSL_CTX_set_options(sslctx, SSL_OP_NO_COMPRESSION);
	}
#endif /* SSL_OP_NO_COMPRESSION */

	SSL_CTX_set_cipher_list(sslctx, ctx->opts->ciphers);
}

/*
 * Create and set up a new SSL_CTX instance for terminating SSL.
 * Set up all the necessary callbacks, the certificate, the cert chain and key.
 */
static SSL_CTX *
pxy_srcsslctx_create(pxy_conn_ctx_t *ctx, X509 *crt, STACK_OF(X509) *chain,
                     EVP_PKEY *key)
{
	SSL_CTX *sslctx = SSL_CTX_new(ctx->opts->sslmethod());
	if (!sslctx)
		return NULL;

	pxy_sslctx_setoptions(sslctx, ctx);

	SSL_CTX_sess_set_new_cb(sslctx, pxy_ossl_sessnew_cb);
	SSL_CTX_sess_set_remove_cb(sslctx, pxy_ossl_sessremove_cb);
	SSL_CTX_sess_set_get_cb(sslctx, pxy_ossl_sessget_cb);
	SSL_CTX_set_session_cache_mode(sslctx, SSL_SESS_CACHE_SERVER |
	                                       SSL_SESS_CACHE_NO_INTERNAL);
#ifdef USE_SSL_SESSION_ID_CONTEXT
	SSL_CTX_set_session_id_context(sslctx, (void *)(&ssl_session_context),
	                                       sizeof(ssl_session_context));
#endif /* USE_SSL_SESSION_ID_CONTEXT */
#ifndef OPENSSL_NO_TLSEXT
	SSL_CTX_set_tlsext_servername_callback(sslctx, pxy_ossl_servername_cb);
	SSL_CTX_set_tlsext_servername_arg(sslctx, ctx);
#endif /* !OPENSSL_NO_TLSEXT */
#ifndef OPENSSL_NO_DH
	if (ctx->opts->dh) {
		SSL_CTX_set_tmp_dh(sslctx, ctx->opts->dh);
	} else {
		SSL_CTX_set_tmp_dh_callback(sslctx, ssl_tmp_dh_callback);
	}
#endif /* !OPENSSL_NO_DH */
#ifndef OPENSSL_NO_ECDH
	if (ctx->opts->ecdhcurve) {
		EC_KEY *ecdh = ssl_ec_by_name(ctx->opts->ecdhcurve);
		SSL_CTX_set_tmp_ecdh(sslctx, ecdh);
		EC_KEY_free(ecdh);
	} else {
		EC_KEY *ecdh = ssl_ec_by_name(NULL);
		SSL_CTX_set_tmp_ecdh(sslctx, ecdh);
		EC_KEY_free(ecdh);
	}
#endif /* !OPENSSL_NO_ECDH */
	SSL_CTX_use_certificate(sslctx, crt);
	SSL_CTX_use_PrivateKey(sslctx, key);
	for (int i = 0; i < sk_X509_num(chain); i++) {
		X509 *c = sk_X509_value(chain, i);
		ssl_x509_refcount_inc(c); /* next call consumes a reference */
		SSL_CTX_add_extra_chain_cert(sslctx, c);
	}

#ifdef DEBUG_SESSION_CACHE
	if (OPTS_DEBUG(ctx->opts)) {
		int mode = SSL_CTX_get_session_cache_mode(sslctx);
		log_dbg_printf("SSL session cache mode: %08x\n", mode);
		if (mode == SSL_SESS_CACHE_OFF)
			log_dbg_printf("SSL_SESS_CACHE_OFF\n");
		if (mode & SSL_SESS_CACHE_CLIENT)
			log_dbg_printf("SSL_SESS_CACHE_CLIENT\n");
		if (mode & SSL_SESS_CACHE_SERVER)
			log_dbg_printf("SSL_SESS_CACHE_SERVER\n");
		if (mode & SSL_SESS_CACHE_NO_AUTO_CLEAR)
			log_dbg_printf("SSL_SESS_CACHE_NO_AUTO_CLEAR\n");
		if (mode & SSL_SESS_CACHE_NO_INTERNAL_LOOKUP)
			log_dbg_printf("SSL_SESS_CACHE_NO_INTERNAL_LOOKUP\n");
		if (mode & SSL_SESS_CACHE_NO_INTERNAL_STORE)
			log_dbg_printf("SSL_SESS_CACHE_NO_INTERNAL_STORE\n");
	}
#endif /* DEBUG_SESSION_CACHE */

	return sslctx;
}

static int
pxy_srccert_write_to_gendir(pxy_conn_ctx_t *ctx, X509 *crt, int is_orig)
{
	char *fn;
	int rv;

	if (!ctx->origcrtfpr)
		return -1;
	if (is_orig) {
		rv = asprintf(&fn, "%s/%s.crt", ctx->opts->certgendir,
		              ctx->origcrtfpr);
	} else {
		if (!ctx->usedcrtfpr)
			return -1;
		rv = asprintf(&fn, "%s/%s-%s.crt", ctx->opts->certgendir,
		              ctx->origcrtfpr, ctx->usedcrtfpr);
	}
	if (rv == -1) {
		ctx->enomem = 1;
		return -1;
	}
	rv = log_cert_submit(fn, crt);
	free(fn);
	return rv;
}

static void
pxy_srccert_write(pxy_conn_ctx_t *ctx)
{
	if (ctx->opts->certgen_writeall || ctx->generated_cert) {
		if (pxy_srccert_write_to_gendir(ctx,
		                SSL_get_certificate(ctx->src.ssl), 0) == -1) {
			log_err_printf("Failed to write used certificate\n");
		}
	}
	if (ctx->opts->certgen_writeall) {
		if (pxy_srccert_write_to_gendir(ctx, ctx->origcrt, 1) == -1) {
			log_err_printf("Failed to write orig certificate\n");
		}
	}
}

static cert_t *
pxy_srccert_create(pxy_conn_ctx_t *ctx)
{
	cert_t *cert = NULL;

	if (ctx->opts->tgcrtdir) {
		if (ctx->sni) {
			cert = cachemgr_tgcrt_get(ctx->sni);
			if (!cert) {
				char *wildcarded;
				wildcarded = ssl_wildcardify(ctx->sni);
				if (!wildcarded) {
					ctx->enomem = 1;
					return NULL;
				}
				cert = cachemgr_tgcrt_get(wildcarded);
				free(wildcarded);
			}
			if (cert && OPTS_DEBUG(ctx->opts)) {
				log_dbg_printf("Target cert by SNI\n");
			}
		} else if (ctx->origcrt) {
			char **names = ssl_x509_names(ctx->origcrt);
			for (char **p = names; *p; p++) {
				if (!cert) {
					cert = cachemgr_tgcrt_get(*p);
				}
				if (!cert) {
					char *wildcarded;
					wildcarded = ssl_wildcardify(*p);
					if (!wildcarded) {
						ctx->enomem = 1;
					} else {
						cert = cachemgr_tgcrt_get(
						       wildcarded);
						free(wildcarded);
					}
				}
				free(*p);
			}
			free(names);
			if (ctx->enomem) {
				return NULL;
			}
			if (cert && OPTS_DEBUG(ctx->opts)) {
				log_dbg_printf("Target cert by origcrt\n");
			}
		}

		if (cert) {
			ctx->immutable_cert = 1;
		}
	}

	if (!cert && ctx->origcrt && ctx->opts->key) {
		cert = cert_new();

		cert->crt = cachemgr_fkcrt_get(ctx->origcrt);
		if (cert->crt) {
			if (OPTS_DEBUG(ctx->opts))
				log_dbg_printf("Certificate cache: HIT\n");
		} else {
			if (OPTS_DEBUG(ctx->opts))
				log_dbg_printf("Certificate cache: MISS\n");
			cert->crt = ssl_x509_forge(ctx->opts->cacrt,
			                           ctx->opts->cakey,
			                           ctx->origcrt, NULL,
			                           ctx->opts->key);
			cachemgr_fkcrt_set(ctx->origcrt, cert->crt);
		}
		cert_set_key(cert, ctx->opts->key);
		cert_set_chain(cert, ctx->opts->chain);
		ctx->generated_cert = 1;
	}

	if ((WANT_CONNECT_LOG(ctx) || ctx->opts->certgendir) && ctx->origcrt) {
		ctx->origcrtfpr = ssl_x509_fingerprint(ctx->origcrt, 0);
		if (!ctx->origcrtfpr)
			ctx->enomem = 1;
	}
	if ((WANT_CONNECT_LOG(ctx) || ctx->opts->certgen_writeall) &&
	    cert && cert->crt) {
		ctx->usedcrtfpr = ssl_x509_fingerprint(cert->crt, 0);
		if (!ctx->usedcrtfpr)
			ctx->enomem = 1;
	}

	return cert;
}

/*
 * Create new SSL context for the incoming connection, based on the original
 * destination SSL certificate.
 * Returns NULL if no suitable certificate could be found.
 */
static SSL *
pxy_srcssl_create(pxy_conn_ctx_t *ctx, SSL *origssl)
{
	cert_t *cert;

	cachemgr_dsess_set((struct sockaddr*)&ctx->addr,
	                   ctx->addrlen, ctx->sni,
	                   SSL_get0_session(origssl));

	ctx->origcrt = SSL_get_peer_certificate(origssl);

	if (OPTS_DEBUG(ctx->opts)) {
		if (ctx->origcrt) {
			log_dbg_printf("===> Original server certificate:\n");
			pxy_debug_crt(ctx->origcrt);
		} else {
			log_dbg_printf("===> Original server has no cert!\n");
		}
	}

	cert = pxy_srccert_create(ctx);
	if (!cert)
		return NULL;

	if (OPTS_DEBUG(ctx->opts)) {
		log_dbg_printf("===> Forged server certificate:\n");
		pxy_debug_crt(cert->crt);
	}

	if (WANT_CONNECT_LOG(ctx)) {
		ctx->ssl_names = ssl_x509_names_to_str(ctx->origcrt ?
		                                       ctx->origcrt :
		                                       cert->crt);
		if (!ctx->ssl_names)
			ctx->enomem = 1;
	}

	SSL_CTX *sslctx = pxy_srcsslctx_create(ctx, cert->crt, cert->chain,
	                                       cert->key);
	cert_free(cert);
	if (!sslctx) {
		ctx->enomem = 1;
		return NULL;
	}
	SSL *ssl = SSL_new(sslctx);
	SSL_CTX_free(sslctx); /* SSL_new() increments refcount */
	if (!ssl) {
		ctx->enomem = 1;
		return NULL;
	}
#ifdef SSL_MODE_RELEASE_BUFFERS
	/* lower memory footprint for idle connections */
	SSL_set_mode(ssl, SSL_get_mode(ssl) | SSL_MODE_RELEASE_BUFFERS);
#endif /* SSL_MODE_RELEASE_BUFFERS */
	return ssl;
}

#ifndef OPENSSL_NO_TLSEXT
/*
 * OpenSSL servername callback, called when OpenSSL receives a servername
 * TLS extension in the clientHello.  Must switch to a new SSL_CTX with
 * a different certificate if we want to replace the server cert here.
 * We generate a new certificate if the current one does not match the
 * supplied servername.  This should only happen if the original destination
 * server supplies a certificate which does not match the server name we
 * indicate to it.
 */
static int
pxy_ossl_servername_cb(SSL *ssl, UNUSED int *al, void *arg)
{
	pxy_conn_ctx_t *ctx = arg;
	const char *sn;
	X509 *sslcrt;

	if (!(sn = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name)))
		return SSL_TLSEXT_ERR_NOACK;

	if (!ctx->sni) {
		if (OPTS_DEBUG(ctx->opts)) {
			log_dbg_printf("Warning: SNI parser yielded no "
			               "hostname, copying OpenSSL one: "
			               "[NULL] != [%s]\n", sn);
		}
		ctx->sni = strdup(sn);
		if (!ctx->sni) {
			ctx->enomem = 1;
			return SSL_TLSEXT_ERR_NOACK;
		}
	}
	if (OPTS_DEBUG(ctx->opts)) {
		if (!!strcmp(sn, ctx->sni)) {
			/*
			 * This may happen if the client resumes a session, but
			 * uses a different SNI hostname when resuming than it
			 * used when the session was created.  OpenSSL
			 * correctly ignores the SNI in the ClientHello in this
			 * case, but since we have already sent the SNI onwards
			 * to the original destination, there is no way back.
			 * We log an error and hope this never happens.
			 */
			log_dbg_printf("Warning: SNI parser yielded different "
			               "hostname than OpenSSL callback for "
			               "the same ClientHello message: "
			               "[%s] != [%s]\n", ctx->sni, sn);
		}
	}

	/* generate a new certificate with sn as additional altSubjectName
	 * and replace it both in the current SSL ctx and in the cert cache */
	if (!ctx->immutable_cert &&
	    !ssl_x509_names_match((sslcrt = SSL_get_certificate(ssl)), sn)) {
		X509 *newcrt;
		SSL_CTX *newsslctx;

		if (OPTS_DEBUG(ctx->opts)) {
			log_dbg_printf("Certificate cache: UPDATE "
			               "(SNI mismatch)\n");
		}
		newcrt = ssl_x509_forge(ctx->opts->cacrt, ctx->opts->cakey,
		                        sslcrt, sn, ctx->opts->key);
		if (!newcrt) {
			ctx->enomem = 1;
			return SSL_TLSEXT_ERR_NOACK;
		}
		cachemgr_fkcrt_set(ctx->origcrt, newcrt);
		ctx->generated_cert = 1;
		if (OPTS_DEBUG(ctx->opts)) {
			log_dbg_printf("===> Updated forged server "
			               "certificate:\n");
			pxy_debug_crt(newcrt);
		}
		if (WANT_CONNECT_LOG(ctx)) {
			if (ctx->ssl_names) {
				free(ctx->ssl_names);
			}
			ctx->ssl_names = ssl_x509_names_to_str(newcrt);
			if (!ctx->ssl_names) {
				ctx->enomem = 1;
			}
		}
		if (WANT_CONNECT_LOG(ctx) || ctx->opts->certgendir) {
			if (ctx->usedcrtfpr) {
				free(ctx->usedcrtfpr);
			}
			ctx->usedcrtfpr = ssl_x509_fingerprint(newcrt, 0);
			if (!ctx->usedcrtfpr) {
				ctx->enomem = 1;
			}
		}

		newsslctx = pxy_srcsslctx_create(ctx, newcrt, ctx->opts->chain,
		                                 ctx->opts->key);
		if (!newsslctx) {
			X509_free(newcrt);
			ctx->enomem = 1;
			return SSL_TLSEXT_ERR_NOACK;
		}
		SSL_set_SSL_CTX(ssl, newsslctx); /* decr's old incr new refc */
		SSL_CTX_free(newsslctx);
		X509_free(newcrt);
	} else if (OPTS_DEBUG(ctx->opts)) {
		log_dbg_printf("Certificate cache: KEEP (SNI match or "
		               "target mode)\n");
	}

	return SSL_TLSEXT_ERR_OK;
}
#endif /* !OPENSSL_NO_TLSEXT */

/*
 * Create new SSL context for outgoing connections to the original destination.
 * If hostname sni is provided, use it for Server Name Indication.
 */
static SSL *
pxy_dstssl_create(pxy_conn_ctx_t *ctx)
{
	SSL_CTX *sslctx;
	SSL *ssl;
	SSL_SESSION *sess;

	sslctx = SSL_CTX_new(ctx->opts->sslmethod());
	if (!sslctx) {
		ctx->enomem = 1;
		return NULL;
	}

	pxy_sslctx_setoptions(sslctx, ctx);

	SSL_CTX_set_verify(sslctx, SSL_VERIFY_NONE, NULL);

	ssl = SSL_new(sslctx);
	SSL_CTX_free(sslctx); /* SSL_new() increments refcount */
	if (!ssl) {
		ctx->enomem = 1;
		return NULL;
	}
#ifndef OPENSSL_NO_TLSEXT
	if (ctx->sni) {
		SSL_set_tlsext_host_name(ssl, ctx->sni);
	}
#endif /* !OPENSSL_NO_TLSEXT */

#ifdef SSL_MODE_RELEASE_BUFFERS
	/* lower memory footprint for idle connections */
	SSL_set_mode(ssl, SSL_get_mode(ssl) | SSL_MODE_RELEASE_BUFFERS);
#endif /* SSL_MODE_RELEASE_BUFFERS */

	/* session resuming based on remote endpoint address and port */
	sess = cachemgr_dsess_get((struct sockaddr *)&ctx->addr,
	                          ctx->addrlen, ctx->sni); /* new sess inst */
	if (sess) {
		if (OPTS_DEBUG(ctx->opts)) {
			log_dbg_printf("Attempt reuse dst SSL session\n");
		}
		SSL_set_session(ssl, sess); /* increments sess refcount */
		SSL_SESSION_free(sess);
	}

	return ssl;
}

/*
 * Free bufferenvent and close underlying socket properly.
 * For OpenSSL bufferevents, this will shutdown the SSL connection.
 */
static void
bufferevent_free_and_close_fd(struct bufferevent *bev, pxy_conn_ctx_t *ctx)
{
	evutil_socket_t fd = bufferevent_getfd(bev);
	SSL *ssl = NULL;

	if (ctx->spec->ssl && !ctx->passthrough) {
		ssl = bufferevent_openssl_get_ssl(bev); /* does not inc refc */
	}

#ifdef DEBUG_PROXY
	if (OPTS_DEBUG(ctx->opts)) {
		log_dbg_printf("            %p free_and_close_fd\n",
		               (void*)bev);
	}
#endif /* DEBUG_PROXY */

	bufferevent_free(bev); /* does not free SSL unless the option
	                          BEV_OPT_CLOSE_ON_FREE was set */
	if (ssl) {
		pxy_ssl_shutdown(ctx->opts, ctx->evbase, ssl, fd);
	} else {
//		evutil_closesocket(fd);
		if (evutil_closesocket(fd) == -1) {
			log_dbg_printf(">############################# bufferevent_free_and_close_fd: evutil_closesocket FAILED, fd=%d\n", fd);
		} else {
			log_dbg_printf(">############################# bufferevent_free_and_close_fd: evutil_closesocket SUCCESS, fd=%d\n", fd);
		}
	}
}

/*
 * Set up a bufferevent structure for either a dst or src connection,
 * optionally with or without SSL.  Sets all callbacks, enables read
 * and write events, but does not call bufferevent_socket_connect().
 *
 * For dst connections, pass -1 as fd.  Pass a pointer to an initialized
 * SSL struct as ssl if the connection should use SSL.
 *
 * Returns pointer to initialized bufferevent structure, as returned
 * by bufferevent_socket_new() or bufferevent_openssl_socket_new().
 */
static struct bufferevent *
pxy_bufferevent_setup(pxy_conn_ctx_t *ctx, evutil_socket_t fd, SSL *ssl)
{
	struct bufferevent *bev;

	log_dbg_printf(">>>>> pxy_bufferevent_setup(): ENTER fd=%d\n", (int)fd);

	if (ssl) {
		log_dbg_printf(">>>>> pxy_bufferevent_setup(): bufferevent_openssl_socket_new <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< SSL\n");
		bev = bufferevent_openssl_socket_new(ctx->evbase, fd, ssl,
				((fd == -1) ? BUFFEREVENT_SSL_CONNECTING : BUFFEREVENT_SSL_ACCEPTING),
				BEV_OPT_DEFER_CALLBACKS);
	} else {
		bev = bufferevent_socket_new(ctx->evbase, fd, BEV_OPT_DEFER_CALLBACKS);
//		bev = bufferevent_socket_new(ctx->evbase, fd, BEV_OPT_DEFER_CALLBACKS|BEV_OPT_THREADSAFE);
	}
	if (!bev) {
		log_err_printf("Error creating bufferevent socket\n");
		return NULL;
	}
#if LIBEVENT_VERSION_NUMBER >= 0x02010000
	if (ssl) {
		log_dbg_printf(">>>>> pxy_bufferevent_setup(): bufferevent_openssl_set_allow_dirty_shutdown <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< SSL\n");
		/* Prevent unclean (dirty) shutdowns to cause error
		 * events on the SSL socket bufferevent. */
		bufferevent_openssl_set_allow_dirty_shutdown(bev, 1);
	}
#endif /* LIBEVENT_VERSION_NUMBER >= 0x02010000 */
	bufferevent_setcb(bev, pxy_bev_readcb, pxy_bev_writecb,
	                  pxy_bev_eventcb, ctx);
//	bufferevent_enable(bev, EV_READ|EV_WRITE);
#ifdef DEBUG_PROXY
	if (OPTS_DEBUG(ctx->opts)) {
		log_dbg_printf("            %p pxy_bufferevent_setup\n",
		               (void*)bev);
	}
#endif /* DEBUG_PROXY */
	log_dbg_printf(">>>>> pxy_bufferevent_setup(): EXIT fd=%d, bev fd=%d\n", (int)fd, bufferevent_getfd(bev));
	return bev;
}

static struct bufferevent *
pxy_bufferevent_setup_e2(pxy_conn_ctx_t *ctx, evutil_socket_t fd, SSL *ssl)
{
	struct bufferevent *bev;

	log_dbg_printf(">>>>> pxy_bufferevent_setup_e2(): ENTER %d\n", (int)fd);

//	bev = bufferevent_socket_new(ctx->evbase, fd, BEV_OPT_DEFER_CALLBACKS);
////	bev = bufferevent_socket_new(ctx->evbase, fd, BEV_OPT_DEFER_CALLBACKS|BEV_OPT_THREADSAFE);

	if (ssl) {
		bev = bufferevent_openssl_socket_new(ctx->evbase, fd, ssl,
				((fd == -1) ? BUFFEREVENT_SSL_CONNECTING : BUFFEREVENT_SSL_ACCEPTING), BEV_OPT_DEFER_CALLBACKS);
	} else {
		bev = bufferevent_socket_new(ctx->evbase, fd, BEV_OPT_DEFER_CALLBACKS);
//		bev = bufferevent_socket_new(ctx->evbase, fd, BEV_OPT_DEFER_CALLBACKS|BEV_OPT_THREADSAFE);
	}
	if (!bev) {
		log_err_printf("Error creating bufferevent socket\n");
		return NULL;
	}

#ifdef DEBUG_PROXY
	if (OPTS_DEBUG(ctx->opts)) {
		log_dbg_printf("            %p pxy_bufferevent_setup_e2\n",
		               (void*)bev);
	}
#endif /* DEBUG_PROXY */
	log_dbg_printf(">>>>> pxy_bufferevent_setup_e2(): EXIT %d\n", (int)fd);
	return bev;
}

/*
 * Filter a single line of HTTP request headers.
 * Also fills in some context fields for logging.
 *
 * Returns NULL if the current line should be deleted from the request.
 * Returns a newly allocated string if the current line should be replaced.
 * Returns `line' if the line should be kept.
 */
static char *
pxy_http_reqhdr_filter_line(const char *line, pxy_conn_ctx_t *ctx)
{
	/* parse information for connect log */
	if (!ctx->http_method) {
		/* first line */
		char *space1, *space2;

		space1 = strchr(line, ' ');
		space2 = space1 ? strchr(space1 + 1, ' ') : NULL;
		if (!space1) {
			/* not HTTP */
			ctx->seen_req_header = 1;
		} else {
			ctx->http_method = malloc(space1 - line + 1);
			if (ctx->http_method) {
				memcpy(ctx->http_method, line, space1 - line);
				ctx->http_method[space1 - line] = '\0';
			} else {
				ctx->enomem = 1;
				return NULL;
			}
			space1++;
			if (!space2) {
				/* HTTP/0.9 */
				ctx->seen_req_header = 1;
				space2 = space1 + strlen(space1);
			}
			ctx->http_uri = malloc(space2 - space1 + 1);
			if (ctx->http_uri) {
				memcpy(ctx->http_uri, space1, space2 - space1);
				ctx->http_uri[space2 - space1] = '\0';
			} else {
				ctx->enomem = 1;
				return NULL;
			}
		}
	} else {
		/* not first line */
		char *newhdr;

		if (!ctx->http_host && !strncasecmp(line, "Host:", 5)) {
			ctx->http_host = strdup(util_skipws(line + 5));
			if (!ctx->http_host) {
				ctx->enomem = 1;
				return NULL;
			}
		} else if (!strncasecmp(line, "Content-Type:", 13)) {
			ctx->http_content_type = strdup(util_skipws(line + 13));
			if (!ctx->http_content_type) {
				ctx->enomem = 1;
				return NULL;
			}
		} else if (!strncasecmp(line, "Connection:", 11)) {
			ctx->sent_http_conn_close = 1;
			if (!(newhdr = strdup("Connection: close"))) {
				ctx->enomem = 1;
				return NULL;
			}
			return newhdr;
		} else if (!strncasecmp(line, "Accept-Encoding:", 16) ||
		           !strncasecmp(line, "Keep-Alive:", 11)) {
			return NULL;
		} else if (line[0] == '\0') {
			ctx->seen_req_header = 1;
			if (!ctx->sent_http_conn_close) {
				newhdr = strdup("Connection: close\r\n");
				if (!newhdr) {
					ctx->enomem = 1;
					return NULL;
				}
				return newhdr;
			}
		}
	}

	return (char*)line;
}

/*
 * Filter a single line of HTTP response headers.
 *
 * Returns NULL if the current line should be deleted from the response.
 * Returns a newly allocated string if the current line should be replaced.
 * Returns `line' if the line should be kept.
 */
static char *
pxy_http_resphdr_filter_line(const char *line, pxy_conn_ctx_t *ctx)
{
	/* parse information for connect log */
	if (!ctx->http_status_code) {
		/* first line */
		char *space1, *space2;

		space1 = strchr(line, ' ');
		space2 = space1 ? strchr(space1 + 1, ' ') : NULL;
		if (!space1 || !!strncmp(line, "HTTP", 4)) {
			/* not HTTP or HTTP/0.9 */
			ctx->seen_resp_header = 1;
		} else {
			size_t len_code, len_text;

			if (space2) {
				len_code = space2 - space1 - 1;
				len_text = strlen(space2 + 1);
			} else {
				len_code = strlen(space1 + 1);
				len_text = 0;
			}
			ctx->http_status_code = malloc(len_code + 1);
			ctx->http_status_text = malloc(len_text + 1);
			if (!ctx->http_status_code || !ctx->http_status_text) {
				ctx->enomem = 1;
				return NULL;
			}
			memcpy(ctx->http_status_code, space1 + 1, len_code);
			ctx->http_status_code[len_code] = '\0';
			if (space2) {
				memcpy(ctx->http_status_text,
				       space2 + 1, len_text);
			}
			ctx->http_status_text[len_text] = '\0';
		}
	} else {
		/* not first line */
		if (!ctx->http_content_length &&
		    !strncasecmp(line, "Content-Length:", 15)) {
			ctx->http_content_length =
				strdup(util_skipws(line + 15));
			if (!ctx->http_content_length) {
				ctx->enomem = 1;
				return NULL;
			}
		} else if (
		    /* HPKP: Public Key Pinning Extension for HTTP
		     * (draft-ietf-websec-key-pinning)
		     * remove to prevent public key pinning */
		    !strncasecmp(line, "Public-Key-Pins:", 16) ||
		    !strncasecmp(line, "Public-Key-Pins-Report-Only:", 28) ||
		    /* HSTS: HTTP Strict Transport Security (RFC 6797)
		     * remove to allow users to accept bad certs */
		    !strncasecmp(line, "Strict-Transport-Security:", 26) ||
		    /* Alternate Protocol
		     * remove to prevent switching to QUIC, SPDY et al */
		    !strncasecmp(line, "Alternate-Protocol:", 19)) {
			return NULL;
		} else if (line[0] == '\0') {
			ctx->seen_resp_header = 1;
		}
	}

	return (char*)line;
}

/*
 * Return 1 if uri is an OCSP GET URI, 0 if not.
 */
static int
pxy_ocsp_is_valid_uri(const char *uri, pxy_conn_ctx_t *ctx)
{
	char *buf_url;
	size_t sz_url;
	char *buf_b64;
	size_t sz_b64;
	unsigned char *buf_asn1;
	size_t sz_asn1;
	int ret;

	buf_url = strrchr(uri, '/');
	if (!buf_url)
		return 0;
	buf_url++;

	/*
	 * Do some quick checks to avoid unnecessary buffer allocations and
	 * decoding URL, Base64 and ASN.1:
	 * -   OCSP requests begin with a SEQUENCE (0x30), so the first Base64
	 *     byte is 'M' or, unlikely but legal, the URL encoding thereof.
	 * -   There should be no query string in OCSP GET requests.
	 * -   Encoded OCSP request ASN.1 blobs are longer than 32 bytes.
	 */
	if (buf_url[0] != 'M' && buf_url[0] != '%')
		return 0;
	if (strchr(uri, '?'))
		return 0;
	sz_url = strlen(buf_url);
	if (sz_url < 32)
		return 0;
	buf_b64 = url_dec(buf_url, sz_url, &sz_b64);
	if (!buf_b64) {
		ctx->enomem = 1;
		return 0;
	}
	buf_asn1 = base64_dec(buf_b64, sz_b64, &sz_asn1);
	if (!buf_asn1) {
		ctx->enomem = 1;
		free(buf_b64);
		return 0;
	}
	ret = ssl_is_ocspreq(buf_asn1, sz_asn1);
	free(buf_asn1);
	free(buf_b64);
	return ret;
}

/*
 * Called after a request header was completely read.
 * If the request is an OCSP request, deny the request by sending an
 * OCSP response of type tryLater and close the connection to the server.
 *
 * Reference:
 * RFC 2560: X.509 Internet PKI Online Certificate Status Protocol (OCSP)
 */
static void
pxy_ocsp_deny(pxy_conn_ctx_t *ctx)
{
	struct evbuffer *inbuf, *outbuf;
	static const char ocspresp[] =
		"HTTP/1.0 200 OK\r\n"
		"Content-Type: application/ocsp-response\r\n"
		"Content-Length: 5\r\n"
		"Connection: close\r\n"
		"\r\n"
		"\x30\x03"      /* OCSPResponse: SEQUENCE */
		"\x0a\x01"      /* OCSPResponseStatus: ENUMERATED */
		"\x03";         /* tryLater (3) */

	if (!ctx->http_method)
		return;
	if (!strncasecmp(ctx->http_method, "GET", 3) &&
	    pxy_ocsp_is_valid_uri(ctx->http_uri, ctx))
		goto deny;
	if (!strncasecmp(ctx->http_method, "POST", 4) &&
	    ctx->http_content_type &&
	    !strncasecmp(ctx->http_content_type,
	                 "application/ocsp-request", 24))
		goto deny;
	return;

deny:
	inbuf = bufferevent_get_input(ctx->src.bev);
	outbuf = bufferevent_get_output(ctx->src.bev);

	if (evbuffer_get_length(inbuf) > 0) {
		if (WANT_CONTENT_LOG(ctx)) {
			logbuf_t *lb;
			lb = logbuf_new_alloc(evbuffer_get_length(inbuf),
			                      NULL, NULL);
			if (lb &&
			    (evbuffer_copyout(inbuf, lb->buf, lb->sz) != -1)) {
				if (log_content_submit(ctx->logctx, lb,
				                       1/*req*/) == -1) {
					logbuf_free(lb);
					log_err_printf("Warning: Content log "
					               "submission failed\n");
				}
			}
		}
		evbuffer_drain(inbuf, evbuffer_get_length(inbuf));
	}
	bufferevent_free_and_close_fd(ctx->dst.bev, ctx);
	ctx->dst.bev = NULL;
	ctx->dst.closed = 1;
	evbuffer_add_printf(outbuf, ocspresp);
	ctx->ocsp_denied = 1;
	if (WANT_CONTENT_LOG(ctx)) {
		logbuf_t *lb;
		lb = logbuf_new_copy(ocspresp, sizeof(ocspresp) - 1,
		                     NULL, NULL);
		if (lb) {
			if (log_content_submit(ctx->logctx, lb,
			                       0/*resp*/) == -1) {
				logbuf_free(lb);
				log_err_printf("Warning: Content log "
				               "submission failed\n");
			}
		}
	}
}

/*
 * Peek into pending data to see if it is an SSL/TLS ClientHello, and if so,
 * upgrade the connection from plain TCP to SSL/TLS.
 *
 * Return 1 if ClientHello was found and connection was upgraded to SSL/TLS,
 * 0 otherwise.
 *
 * WARNING: This is experimental code and will need to be improved.
 *
 * TODO - enable search and skip bytes before ClientHello in case it does not
 *        start at offset 0 (i.e. chello > vec_out[0].iov_base)
 * TODO - peek into more than just the current segment
 * TODO - add retry mechanism for short truncated ClientHello, possibly generic
 */
int
pxy_conn_autossl_peek_and_upgrade(pxy_conn_ctx_t *ctx)
{
	struct evbuffer *inbuf;
	struct evbuffer_iovec vec_out[1];
	const unsigned char *chello;
	if (OPTS_DEBUG(ctx->opts)) {
		log_dbg_printf("Checking for a client hello\n");
	}
	/* peek the buffer */
	inbuf = bufferevent_get_input(ctx->src.bev);
	if (evbuffer_peek(inbuf, 1024, 0, vec_out, 1)) {
		if (ssl_tls_clienthello_parse(vec_out[0].iov_base,
		                              vec_out[0].iov_len,
		                              0, &chello, &ctx->sni) == 0) {
			if (OPTS_DEBUG(ctx->opts)) {
				log_dbg_printf("Peek found ClientHello\n");
			}
			ctx->dst.ssl = pxy_dstssl_create(ctx);
			if (!ctx->dst.ssl) {
				log_err_printf("Error creating SSL for "
				               "upgrade\n");
				return 0;
			}
			ctx->dst.bev = bufferevent_openssl_filter_new(
			               ctx->evbase, ctx->dst.bev, ctx->dst.ssl,
			               BUFFEREVENT_SSL_CONNECTING, 0);
			bufferevent_setcb(ctx->dst.bev, pxy_bev_readcb,
			                  pxy_bev_writecb, pxy_bev_eventcb,
			                  ctx);
			log_dbg_printf(">>>>>----------------------- pxy_conn_autossl_peek_and_upgrade(): bufferevent_enable\n");
			bufferevent_enable(ctx->dst.bev, EV_READ|EV_WRITE);
			if(!ctx->dst.bev) {
				return 0;
			}
			if( OPTS_DEBUG(ctx->opts)) {
				log_err_printf("Replaced dst bufferevent, new "
				               "one is %p\n", ctx->dst.bev);
			}
			ctx->clienthello_search = 0;
			ctx->clienthello_found = 1;
			return 1;
		} else {
			if (OPTS_DEBUG(ctx->opts)) {
				log_dbg_printf("Peek found no ClientHello\n");
			}
			return 0;
		}
	}
	return 0;
}

void
pxy_conn_terminate_free(pxy_conn_ctx_t *ctx)
{
	log_err_printf("Terminating connection%s!\n",
	               ctx->enomem ? " (out of memory)" : "");
	if (ctx->dst.bev && !ctx->dst.closed) {
		bufferevent_free_and_close_fd(ctx->dst.bev, ctx);
		ctx->dst.bev = NULL;
	}
	if (ctx->src.bev && !ctx->src.closed) {
		bufferevent_free_and_close_fd(ctx->src.bev, ctx);
		ctx->src.bev = NULL;
	}
	pxy_conn_ctx_free(ctx);
}

int
pxy_conn_is_ready_to_free(pxy_conn_ctx_t *ctx)
{
	log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free: ENTER fd=%d\n", ctx->fd);

	pxy_conn_desc_t *src = &ctx->src;
	int src_closed = ctx->src_eof;

	pxy_conn_desc_t *e2src = &ctx->e2src;
	int e2src_closed = ctx->e2src_eof;

	int src_inbuf_empty = 1;
	int src_outbuf_empty = 1;
	int e2src_inbuf_empty = 1;
	int e2src_outbuf_empty = 1;

	if ((!src->bev || !e2src->bev) && !src_closed && !e2src_closed) {
		goto not_ready;
	}
	
	if (!src->bev) {
		goto ready;
	}
	
	struct evbuffer *src_inbuf = bufferevent_get_input(src->bev);
	src_inbuf_empty = evbuffer_get_length(src_inbuf) == 0;
	
	struct evbuffer *src_outbuf = bufferevent_get_output(src->bev);
	src_outbuf_empty = evbuffer_get_length(src_outbuf) == 0;

	struct evbuffer *e2src_inbuf = bufferevent_get_input(e2src->bev);
	e2src_inbuf_empty = evbuffer_get_length(e2src_inbuf) == 0;
	
	struct evbuffer *e2src_outbuf = bufferevent_get_output(e2src->bev);
	e2src_outbuf_empty = evbuffer_get_length(e2src_outbuf) == 0;

	if (!src_closed) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free: src_closed NOT CLOSED fd=%d\n", ctx->fd);
	}
	
	if (!src_inbuf_empty) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free: src_inbuf NOT EMPTY fd=%d\n", ctx->fd);
	}
	
	if (!src_outbuf_empty) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free: src_outbuf NOT EMPTY fd=%d\n", ctx->fd);
	}
	
	if (!e2src_closed) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free: e2src_closed NOT CLOSED fd=%d\n", ctx->fd);
	}
	
	if (!e2src_inbuf_empty) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free: e2src_inbuf NOT EMPTY fd=%d\n", ctx->fd);
	}
	
	if (!e2src_outbuf_empty) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free: e2src_outbuf NOT EMPTY fd=%d\n", ctx->fd);
	}

	if (src_closed && !src_inbuf_empty && !e2src_closed) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free: src_closed && !src_inbuf_empty && !e2src_closed fd=%d\n", ctx->fd);
		goto not_ready;
	}
	
	if (e2src_closed && !e2src_inbuf_empty && !src_closed) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free: e2src_closed && !e2src_inbuf_empty && !src_closed fd=%d\n", ctx->fd);
		goto not_ready;
	}
	
	if (src_closed && e2src_closed && !ctx->initialized) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free: src_closed && e2src_closed && !ctx->initialized fd=%d\n", ctx->fd);
		goto ready;
	}
	
	if (e2src_closed && !ctx->initialized) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free: e2src_closed && !ctx->initialized fd=%d\n", ctx->fd);
		goto ready;
	}
	
	if (src_closed && !ctx->initialized) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free: (src_closed || e2src_closed) && !ctx->initialized fd=%d\n", ctx->fd);
		goto not_ready;
	}
	
	if ((src_closed && e2src_closed) || (src_closed && e2src_outbuf_empty) || (e2src_closed && src_outbuf_empty) ||
			((src_closed || e2src_closed) && ctx->initialized && !ctx->mctx->child_ctx)) {
//			((src_closed || e2src_closed) && ctx->initialized && !ctx->child_ctx)) {
		ctx->src_eof = 1;
		ctx->e2src_eof = 1;
		goto ready;
	}

not_ready:
	log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free: EXIT NOT READY, fd=%d\n", ctx->fd);
	return 0;

ready:
	if (!src_inbuf_empty) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free: src_inbuf DRAIN fd=%d\n", ctx->fd);
		evbuffer_drain(src_inbuf, evbuffer_get_length(src_inbuf));
	}
	
	if (!src_outbuf_empty) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free: src_outbuf DRAIN fd=%d\n", ctx->fd);
		evbuffer_drain(src_outbuf, evbuffer_get_length(src_outbuf));

//		size_t packet_size = evbuffer_get_length(src_outbuf);
//		char *packet = malloc(packet_size);
//
//		int bytes_read = evbuffer_remove(src_outbuf, packet, packet_size);
//		if (bytes_read < 0) {
//			log_err_printf("ERROR: evbuffer_remove cannot drain the buffer\n");
//		}
//
//		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free: DRAIN src_outbuf packet (size = %d):\n%.*s\n",
//				(int) packet_size, (int) packet_size, packet);
//
//		if (evbuffer_get_length(src_outbuf) > 0) {
//			if (evbuffer_drain(src_outbuf, evbuffer_get_length(src_outbuf)) == -1) {
//				log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free: src_outbuf CANNOT DRAIN fd=%d\n", ctx->fd);
//			} 
//		}
	}
	
	if (!e2src_inbuf_empty) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free: e2src_inbuf DRAIN fd=%d\n", ctx->fd);
		evbuffer_drain(e2src_inbuf, evbuffer_get_length(e2src_inbuf));
	}
	
	if (!e2src_outbuf_empty) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free: e2src_outbuf DRAIN fd=%d\n", ctx->fd);
		evbuffer_drain(e2src_outbuf, evbuffer_get_length(e2src_outbuf));
	}
					   
	log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free: EXIT READY, fd=%d\n", ctx->fd);
	return 1;
}

int
pxy_conn_is_ready_to_free_e2(pxy_conn_ctx_t *ctx)
{
	log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free_e2: ENTER fd=%d\n", ctx->fd);

//	pxy_conn_ctx_t *parent_ctx = ctx->parent_ctx;
	pxy_conn_ctx_t *parent_ctx;
	if (ctx->mctx) {
		parent_ctx = ctx->mctx->parent_ctx;
	}

	int e2_closed = ctx->e2dst_eof;

	pxy_conn_desc_t *e2dst = &ctx->e2dst;
	struct evbuffer *e2_inbuf = bufferevent_get_input(e2dst->bev);
	int e2_inbuf_empty = evbuffer_get_length(e2_inbuf) == 0;
	
	struct evbuffer *e2_outbuf = bufferevent_get_output(e2dst->bev);
	int e2_outbuf_empty = evbuffer_get_length(e2_outbuf) == 0;

	int dst_closed = ctx->dst_eof;

	pxy_conn_desc_t *dst = &ctx->dst;
	struct evbuffer *dst_inbuf = bufferevent_get_input(dst->bev);
	int dst_inbuf_empty = evbuffer_get_length(dst_inbuf) == 0;

	struct evbuffer *dst_outbuf = bufferevent_get_output(dst->bev);
	int dst_outbuf_empty = evbuffer_get_length(dst_outbuf) == 0;

	if (!e2_closed) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free_e2: e2_closed NOT CLOSED fd=%d\n", ctx->fd);
	}
	
	if (!e2_inbuf_empty) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free_e2: e2_inbuf NOT EMPTY fd=%d\n", ctx->fd);
	}
	
	if (!e2_outbuf_empty) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free_e2: e2_outbuf NOT EMPTY fd=%d\n", ctx->fd);
	}
	
	if (!dst_closed) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free_e2: dst_closed NOT CLOSED fd=%d\n", ctx->fd);
	}
	
	if (!dst_inbuf_empty) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free_e2: dst_inbuf NOT EMPTY fd=%d\n", ctx->fd);
	}
	
	if (!dst_outbuf_empty) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free_e2: dst_outbuf NOT EMPTY fd=%d\n", ctx->fd);
	}
	
	if ((e2_closed && !e2_inbuf_empty && !dst_closed)) {
		goto not_ready;
	}
	
	if ((dst_closed && !dst_inbuf_empty && !e2_closed)) {
		goto not_ready;
	}
	
	int src_closed = 1;
	int e2src_closed = 1;

	if (parent_ctx) {
		src_closed = parent_ctx->src_eof;
		e2src_closed = parent_ctx->e2src_eof;
	}
	
	if ((e2_closed && dst_closed) || (e2_closed && dst_inbuf_empty && dst_outbuf_empty) || (dst_closed && e2_inbuf_empty && e2_outbuf_empty) ||
			((src_closed || e2src_closed) && e2_inbuf_empty && e2_outbuf_empty && dst_inbuf_empty && dst_outbuf_empty) ||
			!parent_ctx) {
		ctx->dst_eof = 1;
		ctx->e2dst_eof = 1;
		goto ready;
	}

not_ready:
	log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free_e2: EXIT NOT READY, fd=%d\n", ctx->fd);
	return 0;

ready:
	if (!e2_inbuf_empty) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free_e2: e2_inbuf DRAIN fd=%d\n", ctx->fd);
		evbuffer_drain(e2_inbuf, evbuffer_get_length(e2_inbuf));
	}
	
	if (!e2_outbuf_empty) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free_e2: e2_outbuf DRAIN fd=%d\n", ctx->fd);
		evbuffer_drain(e2_outbuf, evbuffer_get_length(e2_outbuf));
	}
	
	if (!dst_inbuf_empty) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free_e2: dst_inbuf DRAIN fd=%d\n", ctx->fd);
		evbuffer_drain(dst_inbuf, evbuffer_get_length(dst_inbuf));
	}
	
	if (!dst_outbuf_empty) {
		log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free_e2: dst_outbuf DRAIN fd=%d\n", ctx->fd);
		evbuffer_drain(dst_outbuf, evbuffer_get_length(dst_outbuf));
	}
					   
	log_dbg_printf(">>>>>>>>>>>>>>>>>>>>>>>>>>>>>> pxy_conn_is_ready_to_free_e2: EXIT READY, fd=%d\n", ctx->fd);
	return 1;
}

static int
pxy_conn_free(pxy_conn_ctx_t *);

void remove_node(pxy_conn_ctx_t *node, pxy_conn_ctx_t **head) {
    if (node->fd == (*head)->fd) {
        *head = (*head)->child_ctx;
        return;
    }

    pxy_conn_ctx_t *current = (*head)->child_ctx;
    pxy_conn_ctx_t *previous = *head;
    while (current != NULL && previous != NULL) {
        if (node->fd == current->fd) {
            previous->child_ctx = current->child_ctx;
            return;
        }
        previous = current;
        current = current->child_ctx;
    }
    return;
}

static int
pxy_conn_free_e2(pxy_conn_ctx_t *ctx)
{
	log_dbg_printf(">############################# pxy_conn_free_e2: ENTER\n");
	evutil_socket_t fd = ctx->fd;

//	pxy_conn_ctx_t *parent_ctx = ctx->parent_ctx;
	pxy_conn_ctx_t *parent_ctx;
	if (ctx->mctx) {
		parent_ctx = ctx->mctx->parent_ctx;
	}
	
	int pfd = -1;
	if (parent_ctx) {
		pfd = parent_ctx->fd;
	}
	
	if (pxy_conn_is_ready_to_free_e2(ctx)) {
		pxy_conn_desc_t *dst = &ctx->dst;
		if (dst->bev) {
			log_dbg_printf(">############################# pxy_conn_free_e2: evutil_closesocket dst->bev, fd=%d\n", bufferevent_getfd(dst->bev));
			bufferevent_free_and_close_fd(dst->bev, ctx);
			dst->bev = NULL;
		}

		pxy_conn_desc_t *e2dst = &ctx->e2dst;
		if (e2dst->bev) {
			log_dbg_printf(">############################# pxy_conn_free_e2: evutil_closesocket e2dst->bev, fd=%d\n", bufferevent_getfd(e2dst->bev));
			bufferevent_free_and_close_fd(e2dst->bev, ctx);
			e2dst->bev = NULL;
		}
		
		int rv = 1;
//		if (parent_ctx) {
		if (ctx->mctx) {
			log_dbg_printf(">############################# pxy_conn_free_e2: remove_node\n");
			remove_node(ctx, &ctx->mctx->child_ctx);
//			remove_node(ctx, &parent_ctx->child_ctx);

			log_dbg_printf(">############################# pxy_conn_free_e2: CHECKING\n");
//			pxy_conn_ctx_t *current_child_ctx = parent_ctx->child_ctx;
			pxy_conn_ctx_t *current_child_ctx = ctx->mctx->child_ctx;
			while (current_child_ctx) {
				log_dbg_printf(">############################# pxy_conn_free_e2: NOT NULL CHILD, fd=%d\n", current_child_ctx->fd);
				current_child_ctx = current_child_ctx->child_ctx;
			}

			if (!ctx->mctx->parent_ctx && !ctx->mctx->child_ctx) {
				log_dbg_printf(">############################# pxy_conn_free_e2: FREEING evcl2, fd=%d, fd2=%d\n", fd, ctx->mctx->fd2);
				evutil_closesocket(ctx->mctx->fd2);
				evconnlistener_free(ctx->mctx->evcl2);

				log_dbg_printf(">############################# pxy_conn_free_e2: RELEASING META CTX, fd=%d, parent fd=%d\n", fd, pfd);
				ctx->mctx->released = 1;
				rv = 2;
			}
		}

		log_dbg_printf(">############################# pxy_conn_free_e2: FREEING CTX, fd=%d, parent fd=%d\n", fd, pfd);

		pxy_conn_ctx_free(ctx);

		log_dbg_printf(">############################# pxy_conn_free_e2: FREED CTX, fd=%d, parent fd=%d\n", fd, pfd);
		return rv;
	} else {
		log_dbg_printf(">############################# pxy_conn_free_e2: CANNOT FREE CTX, fd=%d, parent fd=%d\n", fd, pfd);
		return 0;
	}
}

static int
pxy_conn_free(pxy_conn_ctx_t *ctx)
{
	int fd = ctx->fd;

	if (!ctx->mctx) {
		goto leavefree;
	}
	
	int cfd = -1;
//	if (ctx->child_ctx) {
	if (ctx->mctx->child_ctx) {
		log_dbg_printf(">############################# pxy_conn_free: ctx->child_ctx NOT NULL\n");
		cfd = ctx->mctx->child_ctx->fd;
	}
	
	if (pxy_conn_is_ready_to_free(ctx)) {

		if (ctx->initialized) {
//			pxy_conn_ctx_t *current = ctx->child_ctx;
			pxy_conn_ctx_t *current = ctx->mctx->child_ctx;
			while (current) {
				pxy_conn_ctx_t *next = current->child_ctx;
				if (pxy_conn_free_e2(current)) {
					log_dbg_printf(">############################# pxy_conn_free: FREE child SUCCESS, fd=%d, child fd=%d\n", fd, cfd);
				}
				current = next;
			}
		}

leavefree:
		log_dbg_printf(">############################# pxy_conn_free: TRY FREE ctx->src\n");
		pxy_conn_desc_t *src = &ctx->src;
		if (src->bev) {
			log_dbg_printf(">############################# pxy_conn_free: evutil_closesocket src->bev, fd=%d\n", bufferevent_getfd(src->bev));
			bufferevent_free_and_close_fd(src->bev, ctx);
			src->bev = NULL;
		}

		pxy_conn_desc_t *dst = &ctx->dst;
		if (dst->bev) {
			log_dbg_printf(">############################# pxy_conn_free: evutil_closesocket dst->bev, fd=%d\n", bufferevent_getfd(dst->bev));
			bufferevent_free_and_close_fd(dst->bev, ctx);
			dst->bev = NULL;
		}

		pxy_conn_desc_t *e2src = &ctx->e2src;
		if (e2src->bev) {
			log_dbg_printf(">############################# pxy_conn_free: evutil_closesocket e2src->bev, fd=%d\n", bufferevent_getfd(e2src->bev));
			bufferevent_free_and_close_fd(e2src->bev, ctx);
			e2src->bev = NULL;
		}

		int rv = 1;
		if (ctx->mctx) {
//			if (!ctx->child_ctx) {
//			if (!ctx->mctx->child_ctx) {
//				log_dbg_printf(">############################# pxy_conn_free: FREEING evcl2, fd=%d, fd2=%d\n", fd, ctx->mctx->fd2);
//				evutil_closesocket(ctx->mctx->fd2);
//				evconnlistener_free(ctx->mctx->evcl2);
//			}
//			} else {
//				log_dbg_printf(">############################# pxy_conn_free: NULLING CHILDREN\n");
//				pxy_conn_ctx_t *current_child_ctx = NULL;
////				current_child_ctx = ctx->child_ctx;
//				current_child_ctx = ctx->mctx->child_ctx;
//				while (current_child_ctx) {
//					log_dbg_printf(">############################# pxy_conn_free: NULLING CHILD, fd=%d, child fd=%d\n", fd, current_child_ctx->fd);
//					current_child_ctx->parent_ctx = NULL;
//					current_child_ctx = current_child_ctx->child_ctx;
//				}
//			}
		
			ctx->mctx->parent_ctx = NULL;
			if (!ctx->mctx->child_ctx) {
				log_dbg_printf(">############################# pxy_conn_free: FREEING evcl2, fd=%d, fd2=%d\n", fd, ctx->mctx->fd2);
				evutil_closesocket(ctx->mctx->fd2);
				evconnlistener_free(ctx->mctx->evcl2);

				log_dbg_printf(">############################# pxy_conn_free: RELEASING META CTX, fd=%d, child fd=%d\n", fd, cfd);
				ctx->mctx->released = 1;
				rv = 2;
			}
		}

		log_dbg_printf(">############################# pxy_conn_free: FREEING ctx, fd=%d, child fd=%d\n", fd, cfd);
		pxy_conn_ctx_free(ctx);
		log_dbg_printf(">############################# pxy_conn_free: FREED CTX, fd=%d, child fd=%d\n", fd, cfd);
		return rv;
	} else {
		log_dbg_printf(">############################# pxy_conn_free: CANNOT FREE CTX, fd=%d, child fd=%d\n", fd, cfd);
		return 0;
	}
}

/*
 * Callback for read events on the up- and downstream connection bufferevents.
 * Called when there is data ready in the input evbuffer.
 */
static void
pxy_bev_readcb(struct bufferevent *bev, void *arg)
{
	pxy_conn_ctx_t *ctx = arg;
	
	if (!ctx) {
		log_dbg_printf(">>>>>,,,,,,,,,,,,,,,,,,,,,,, pxy_bev_readcb: ctx NULL\n");
		return;
	}
	if (!ctx->mctx) {
		log_dbg_printf(">>>>>,,,,,,,,,,,,,,,,,,,,,,, pxy_bev_readcb: ctx->mctx NULL\n");
		return;
	}
//	pthread_mutex_t *cmutex = &ctx->thrmgr->mutex2;
	pthread_mutex_t *cmutex = &ctx->mctx->mutex;
	pthread_mutex_lock(cmutex);

	if (!ctx || !ctx->src.bev || !ctx->e2src.bev) {
		log_dbg_printf(">>>>>,,,,,,,,,,,,,,,,,,,,,,, pxy_bev_readcb: NULL ctx || bev <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< GONE\n");
		goto leave;
	}
			
	char event_name[6] = "\0\0\0\0\0\0";
	if (bev == ctx->src.bev) {
		strcpy(event_name, "src");
	} else if (bev == ctx->dst.bev) {
		strcpy(event_name, "dst");
	} else if (bev == ctx->e2src.bev) {
		strcpy(event_name, "e2src");
	} else if (bev == ctx->e2dst.bev) {
		strcpy(event_name, "e2dst");
	} else if (bev == NULL) {
		strcpy(event_name, "NULL");
	} else {
		strcpy(event_name, "UNKWN");
	}
	
	log_dbg_printf(">>>>>,,,,,,,,,,,,,,,,,,,,,,, pxy_bev_readcb: %s, fd=%d\n", event_name, ctx->fd);

	if (bev == ctx->src.bev) {
		if (ctx->clienthello_search) {
			if (pxy_conn_autossl_peek_and_upgrade(ctx)) {
				log_dbg_printf(">>>>>,,,,,,,,,,,,,,,,,,,,,,, pxy_bev_readcb: pxy_conn_autossl_peek_and_upgrade RETURNS\n");
				return;
			}
		}
	
		if (ctx->e2src.bev) {
			struct evbuffer *inbuf = bufferevent_get_input(bev);

			struct sockaddr_in e2listener_addr;
			socklen_t e2listener_len;

			e2listener_len = sizeof(e2listener_addr);
			
//			if (ctx->child_ctx) {
			if (ctx->mctx->child_ctx) {
				// @todo Check why this won't work
				//getsockname(ctx->child_ctx->fd, &e2listener_addr, &e2listener_len);
				log_dbg_printf(">>>>>,,,,,,,,,,,,,,,,,,,,,,, pxy_bev_readcb: %s, CALLING getsockname, fd=%d ,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,, fd_e2=%d\n", event_name, ctx->fd, ctx->mctx->fd2);
				getsockname(ctx->mctx->fd2, &e2listener_addr, &e2listener_len);
			} else {
				log_dbg_printf(">>>>>,,,,,,,,,,,,,,,,,,,,,,, pxy_bev_readcb: %s, FIRST CALL getsockname, fd=%d ,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,,, fd_e2=%d\n", event_name, ctx->fd, ctx->mctx->fd2);
				// @todo Check if the fd is same for all children
				getsockname(ctx->mctx->fd2, &e2listener_addr, &e2listener_len);
			}

			char *addr = inet_ntoa(e2listener_addr.sin_addr);
			int addr_len = strlen(addr) + 5 + 3 + 1;

			char *pxy_dst = malloc(addr_len);
			snprintf(pxy_dst, addr_len, "[%s]:%d", addr, (int) ntohs(e2listener_addr.sin_port));

			char *custom_key = "\r\nSSLproxy-Addr: ";
			size_t custom_field_len = strlen(custom_key) + strlen(pxy_dst) + 1;

			char *custom_field = malloc(custom_field_len);
			snprintf(custom_field, custom_field_len, "%s%s", custom_key, pxy_dst);
			free(pxy_dst);
			
			log_dbg_printf(">>>>>,,,,,,,,,,,,,,,,,,,,,,, pxy_bev_readcb: custom_field= %s\n", custom_field);
			
			size_t packet_size = evbuffer_get_length(inbuf);
			char *packet = malloc(packet_size + custom_field_len);
			if (!packet) {
				ctx->enomem = 1;
				goto leave;
			}

			int bytes_read = evbuffer_remove(inbuf, packet, packet_size);
			if (bytes_read < 0) {
				log_err_printf("ERROR: evbuffer_remove cannot drain the buffer\n");
			}
			
			log_dbg_printf(">>>>>,,,,,,,,,,,,,,,,,,,,,,, pxy_bev_readcb: src ORIG packet (size = %d), fd=%d:\n%.*s\n",
					(int) packet_size, ctx->fd, (int) packet_size, packet);

			packet[packet_size] = '\0';
			packet_size+= custom_field_len;

			char *pos = strstr(packet, "\r\n\r\n");
			if (pos) {
				char *content = strdup(pos);
				int header_len = pos - packet;
				char *header = malloc(header_len + 1);
				strncpy(header, packet, header_len);
				header[header_len] = '\0';
				
				snprintf(packet, packet_size, "%s%s%s", header, custom_field, content);

				free(header);
				free(content);
			}
			free(custom_field);

			struct evbuffer *e2outbuf = bufferevent_get_output(ctx->e2src.bev);

			int add_result = evbuffer_add(e2outbuf, packet, packet_size - 1);
			if (add_result < 0) {
				log_err_printf("ERROR: evbuffer_add failed\n");
			}

			log_dbg_printf(">>>>>,,,,,,,,,,,,,,,,,,,,,,, pxy_bev_readcb: src packet (size = %d), fd=%d:\n%.*s\n",
					(int) packet_size, ctx->fd, (int) packet_size, packet);
//			log_dbg_printf(">>>>>,,,,,,,,,,,,,,,,,,,,,,, pxy_bev_readcb: src packet (size = %d)\n", (int) packet_size);

			free(packet);
		} else {
			log_dbg_printf(">>>>>,,,,,,,,,,,,,,,,,,,,,,, pxy_bev_readcb: src ctx->e2src.bev NULL\n");
		}
	}
	else if (bev == ctx->e2src.bev) {
		if (ctx->src.bev) {
			struct evbuffer *inbuf = bufferevent_get_input(bev);

			size_t packet_size = evbuffer_get_length(inbuf);
			char *packet = malloc(packet_size);
			if (!packet) {
				ctx->enomem = 1;
				goto leave;
			}

			int bytes_read = evbuffer_remove(inbuf, packet, packet_size);
			if (bytes_read < 0) {
				log_err_printf("ERROR: evbuffer_remove cannot drain the buffer\n");
			}

			struct evbuffer *outbuf = bufferevent_get_output(ctx->src.bev);

			int add_result = evbuffer_add(outbuf, packet, packet_size);
			if (add_result < 0) {
				log_err_printf("ERROR: evbuffer_add failed\n");
			}

//			log_dbg_printf(">>>>>,,,,,,,,,,,,,,,,,,,,,,, pxy_bev_readcb: e2src packet (size = %d):\n%.*s\n",
//					(int) packet_size, (int) packet_size, packet);
			log_dbg_printf(">>>>>,,,,,,,,,,,,,,,,,,,,,,, pxy_bev_readcb: e2src packet (size = %d)\n", (int) packet_size);

			free(packet);
		} else {
			log_dbg_printf(">>>>>,,,,,,,,,,,,,,,,,,,,,,, pxy_bev_readcb: e2src ctx->src.bev NULL\n");
		}
	}

leave:
	log_dbg_printf(">>>>>,,,,,,,,,,,,,,,,,,,,,,, pxy_bev_readcb: EXIT\n");
	pthread_mutex_unlock(cmutex);
}

static void
pxy_bev_readcb_e2(struct bufferevent *bev, void *arg)
{
	pxy_conn_ctx_t *ctx = arg;

	if (!ctx) {
		log_dbg_printf(">>>>>....................... pxy_bev_readcb_e2: ctx NULL\n");
		return;
	}
	if (!ctx->mctx) {
		log_dbg_printf(">>>>>....................... pxy_bev_readcb_e2: ctx->mctx NULL\n");
		return;
	}
	
//	pthread_mutex_t *cmutex = &ctx->thrmgr->mutex2;
	pthread_mutex_t *cmutex = &ctx->mctx->mutex;
	pthread_mutex_lock(cmutex);

	if (!ctx || !ctx->e2dst.bev || !ctx->dst.bev) {
		log_dbg_printf(">>>>>....................... pxy_bev_readcb_e2: NULL ctx || bev <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< GONE\n");
		goto leave;
	}
	
//	pxy_conn_ctx_t *parent_ctx = ctx->parent_ctx;
	pxy_conn_ctx_t *parent_ctx = ctx->mctx->parent_ctx;
	evutil_socket_t pfd = -1;
	if (parent_ctx) {
		pfd = parent_ctx->fd;
	}

	char event_name[6] = "\0\0\0\0\0\0";
	if (bev == ctx->src.bev) {
		strcpy(event_name, "src");
	} else if (bev == ctx->dst.bev) {
		strcpy(event_name, "dst");
	} else if (bev == ctx->e2src.bev) {
		strcpy(event_name, "e2src");
	} else if (bev == ctx->e2dst.bev) {
		strcpy(event_name, "e2dst");
	} else if (bev == NULL) {
		strcpy(event_name, "NULL");
	} else {
		strcpy(event_name, "UNKWN");
	}
	
	log_dbg_printf(">>>>>....................... pxy_bev_readcb_e2: %s, fd=%d\n", event_name, ctx->fd);
	
	struct sockaddr_in peeraddr;
	socklen_t peeraddrlen;

	peeraddrlen = sizeof(peeraddr);
	getpeername(ctx->fd, &peeraddr, &peeraddrlen);

	if (bev == ctx->e2dst.bev) {
		if (ctx->dst.bev) {

			log_dbg_printf(">>>>>.................................................................................... pxy_bev_readcb_e2: PEER [%s]:%d <<<<< fd=%d, parent fd=%d\n", inet_ntoa(peeraddr.sin_addr), (int) ntohs(peeraddr.sin_port), ctx->fd, pfd);

			struct evbuffer *e2outbuf = bufferevent_get_input(ctx->e2dst.bev);

			char *custom_key = "SSLproxy-Addr: ";
			struct evbuffer_ptr ebp = evbuffer_search(e2outbuf, custom_key, strlen(custom_key), NULL);
			if (ebp.pos != -1) {
				log_dbg_printf(">>>>>....................... pxy_bev_readcb_e2: evbuffer_search FOUND = %d\n", ebp.pos);
			} else {
				log_dbg_printf(">>>>>....................... pxy_bev_readcb_e2: evbuffer_search FAILED\n");
			}
			
			size_t packet_size = evbuffer_get_length(e2outbuf);
			char *packet = malloc(packet_size);
			if (!packet) {
				ctx->enomem = 1;
				goto leave;
			}

			log_dbg_printf(">>>>>....................... pxy_bev_readcb_e2: packet_size\n");
		
			if (packet_size > 0) {
				int bytes_read = evbuffer_remove(e2outbuf, packet, packet_size);
				if (bytes_read < 0) {
					log_err_printf("ERROR: evbuffer_remove cannot drain the buffer\n");
				}

				log_dbg_printf(">>>>>....................... pxy_bev_readcb_e2: bufferevent_get_output\n");
		
				struct evbuffer *outbuf = bufferevent_get_output(ctx->dst.bev);
				int add_result = evbuffer_add(outbuf, packet, packet_size);
				if (add_result < 0) {
					log_err_printf("ERROR: evbuffer_add failed\n");
				}
				
				log_dbg_printf(">>>>>....................... pxy_bev_readcb_e2: e2dst packet (size = %d), fd=%d, parent fd=%d:\n%.*s\n",
						(int) packet_size, ctx->fd, pfd, (int) packet_size, packet);
//				log_dbg_printf(">>>>>....................... pxy_bev_readcb_e2: e2dst packet (size = %d)\n", (int) packet_size);
			}
			free(packet);
		} else {
			log_dbg_printf(">>>>>....................... pxy_bev_readcb_e2: e2dst ctx->dst.bev NULL\n");
		}
	}
	else if (bev == ctx->dst.bev) {
		if (ctx->e2dst.bev) {
			struct evbuffer *inbuf = bufferevent_get_input(bev);

			size_t packet_size = evbuffer_get_length(inbuf);
			char *packet = malloc(packet_size);
			if (!packet) {
				ctx->enomem = 1;
				goto leave;
//				return;
			}

			int bytes_read = evbuffer_remove(inbuf, packet, packet_size);

			if (bytes_read < 0) {
				log_err_printf("ERROR: evbuffer_remove cannot drain the buffer\n");
			}
			
			struct evbuffer *e2outbuf = bufferevent_get_output(ctx->e2dst.bev);

			int add_result = evbuffer_add(e2outbuf, packet, packet_size);
			if (add_result < 0) {
				log_err_printf("ERROR: evbuffer_add failed\n");
			}

//			log_dbg_printf(">>>>>,,,,,,,,,,,,,,,,,,,,,,, pxy_bev_readcb_e2: dst packet (size = %d):\n%.*s\n",
//					(int) packet_size, (int) packet_size, packet);
			log_dbg_printf(">>>>>,,,,,,,,,,,,,,,,,,,,,,, pxy_bev_readcb_e2: dst packet (size = %d)\n", (int) packet_size);

			free(packet);
		} else {
			log_dbg_printf(">>>>>,,,,,,,,,,,,,,,,,,,,,,, pxy_bev_readcb_e2: dst ctx->e2dst.bev NULL\n");
		}
	}

leave:
	log_dbg_printf(">>>>>....................... pxy_bev_readcb_e2: EXIT\n");
	pthread_mutex_unlock(cmutex);
}

static int
pxy_connected_enable(struct bufferevent *bev, pxy_conn_ctx_t *ctx, char *event_name)
{
	if (!ctx) {
		log_dbg_printf(">>>>>=================================== pxy_connected_enable: ctx NULL\n");
		return 0;
	}
	
	log_dbg_printf(">>>>>=================================== pxy_connected_enable: CONNECTED %s fd=%d\n", event_name, ctx->fd);

	if (bev == ctx->dst.bev && !ctx->dst_connected) {
		ctx->dst_connected = 1;

		log_dbg_printf(">>>>>=================================== pxy_connected_enable: pxy_bufferevent_setup for e2src fd=%d\n", ctx->fd);
		ctx->e2src.ssl= NULL;
		ctx->e2src.bev = pxy_bufferevent_setup(ctx, -1, ctx->e2src.ssl);

		log_dbg_printf(">>>>>=================================== pxy_connected_enable: bufferevent_socket_connect for e2src fd=%d\n", ctx->fd);
		if (bufferevent_socket_connect(ctx->e2src.bev,
								   (struct sockaddr *)&ctx->spec->e2src_addr,
								   ctx->spec->e2src_addrlen) == -1) {
			log_dbg_printf(">>>>>=================================== pxy_connected_enable: FAILED bufferevent_socket_connect: e2src\n");
		}
		bufferevent_enable(ctx->e2src.bev, EV_READ|EV_WRITE);
	}

	if (bev == ctx->e2src.bev && !ctx->e2src_connected) {
		ctx->e2src_connected = 1;
	}

	if (ctx->dst_connected && ctx->e2src_connected && !ctx->connected) {
		ctx->connected = 1;

		pxy_conn_desc_t *dst_ctx = &ctx->dst;
		if ((ctx->spec->ssl || ctx->clienthello_found) && !ctx->passthrough) {
			log_dbg_printf(">>>>>=================================== pxy_srcssl_create <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< SSL\n");
			ctx->src.ssl = pxy_srcssl_create(ctx, dst_ctx->ssl);
//			if (!ctx->src.ssl) {
//				bufferevent_free_and_close_fd(bev, ctx);
//				ctx->dst.bev = NULL;
//				ctx->dst.ssl = NULL;
////				if (ctx->opts->passthrough && !ctx->enomem) {
////					ctx->passthrough = 1;
////					ctx->connected = 0;
////					log_dbg_printf("No cert found; "
////					               "falling back "
////					               "to passthrough\n");
////					pxy_fd_readcb(ctx->fd, 0, ctx);
////					return;
////				}
//				evutil_closesocket(ctx->fd);
//				pxy_conn_ctx_free(ctx);
//				return;
//			}
		}
		if (ctx->clienthello_found) {
			if (OPTS_DEBUG(ctx->opts)) {
				log_dbg_printf(">>>>>=================================== pxy_connected_enable: Completing autossl upgrade\n");
			}
			ctx->src.bev = bufferevent_openssl_filter_new(
			               ctx->evbase, ctx->src.bev, ctx->src.ssl,
			               BUFFEREVENT_SSL_ACCEPTING,
			               BEV_OPT_DEFER_CALLBACKS);
			bufferevent_setcb(ctx->src.bev, pxy_bev_readcb,
			                  pxy_bev_writecb, pxy_bev_eventcb,
			                  ctx);
//			bufferevent_enable(ctx->src.bev, EV_READ|EV_WRITE);
		} else {
//			ctx->src.bev = pxy_bufferevent_setup(ctx, ctx->fd,
//			                                     ctx->src.ssl);
			log_dbg_printf(">>>>>=================================== pxy_connected_enable: SETUP src.bev fd=%d\n", ctx->fd);
			ctx->src.bev = pxy_bufferevent_setup(ctx, ctx->fd, ctx->src.ssl);
			if (!ctx->src.bev) {
				log_dbg_printf(">>>>>=================================== pxy_connected_enable: src.bev NULL FREEING\n");

				bufferevent_free_and_close_fd(bev, ctx);
				evutil_closesocket(ctx->fd);
				pxy_conn_ctx_free(ctx);
				return 0;
			}
		}

		bufferevent_enable(ctx->src.bev, EV_READ|EV_WRITE);
	}

	return 1;
}

/*
 * Callback for write events on the up- and downstream connection bufferevents.
 * Called when either all data from the output evbuffer has been written,
 * or if the outbuf is only half full again after having been full.
 */
static void
pxy_bev_writecb(struct bufferevent *bev, void *arg)
{
	pxy_conn_ctx_t *ctx = arg;
	if (!ctx) {
		log_dbg_printf(">>>>>+++++++++++++++++++++++++++++++++++ pxy_bev_writecb: ctx NULL\n");
		return;
	}
	if (!ctx->mctx) {
		log_dbg_printf(">>>>>+++++++++++++++++++++++++++++++++++ pxy_bev_writecb: ctx->mctx NULL\n");
		return;
	}

//	pthread_mutex_t *cmutex = &ctx->thrmgr->mutex2;
	proxy_conn_meta_ctx_t *mctx = ctx->mctx;
	pthread_mutex_t *cmutex = &mctx->mutex;
	pthread_mutex_lock(cmutex);
	
	int rv = 0;

	char event_name[6] = "\0\0\0\0\0\0";
	if (bev == ctx->src.bev) {
		strcpy(event_name, "src");
	} else if (bev == ctx->dst.bev) {
		strcpy(event_name, "dst");
	} else if (bev == ctx->e2src.bev) {
		strcpy(event_name, "e2src");
	} else if (bev == ctx->e2dst.bev) {
		strcpy(event_name, "e2dst");
	} else if (bev == NULL) {
		strcpy(event_name, "NULL");
	} else {
		strcpy(event_name, "UNKWN");
		log_dbg_printf(">>>>>+++++++++++++++++++++++++++++++++++ pxy_bev_writecb: event_name == UNKWN <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< NOT INIT\n");
		goto leave;
	}
	
	log_dbg_printf(">>>>>+++++++++++++++++++++++++++++++++++ pxy_bev_writecb: %s, %d\n", event_name, ctx->fd);

	// XXX: For Squid's Zero Sized Reply
	if ((bev == ctx->dst.bev) && !ctx->dst_connected) {
		// @attention Do not call pxy_bev_eventcb() instead, that would cause deadlock
		//pxy_bev_eventcb(bev, BEV_EVENT_CONNECTED, ctx);
		pxy_connected_enable(bev, ctx, event_name);
	}

	if (!ctx) {
		log_dbg_printf(">>>>>+++++++++++++++++++++++++++++++++++ pxy_bev_writecb: NULL ctx || bev <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< GONE\n");
		goto leave;
	}
	
	
	if (ctx->src_eof || ctx->e2src_eof) {
		log_dbg_printf(">>>>>+++++++++++++++++++++++++++++++++++ pxy_bev_writecb(): TRY CLOSING\n");
		rv = pxy_conn_free(ctx);
	}

leave:
	log_dbg_printf(">>>>>+++++++++++++++++++++++++++++++++++ pxy_bev_writecb: EXIT\n");
	if (rv == 2) {
		log_dbg_printf(">>>>>+++++++++++++++++++++++++++++++++++ pxy_bev_writecb: EXIT FREE META CTX\n");
		pthread_mutex_destroy(cmutex);
		free(mctx);
	} else {
		pthread_mutex_unlock(cmutex);
	}
}

static void
pxy_bev_writecb_e2(struct bufferevent *bev, void *arg)
{
	pxy_conn_ctx_t *ctx = arg;

//	assert(ctx != NULL);
	if (!ctx) {
		log_dbg_printf(">>>>>??????????????????????????? pxy_bev_writecb_e2: ctx NULL\n");
		return;
	}
	if (!ctx->mctx) {
		log_dbg_printf(">>>>>??????????????????????????? pxy_bev_writecb_e2: ctx->mctx NULL\n");
		return;
	}

//	pthread_mutex_t *cmutex = &ctx->thrmgr->mutex2;
	proxy_conn_meta_ctx_t *mctx = ctx->mctx;
	pthread_mutex_t *cmutex = &mctx->mutex;
	pthread_mutex_lock(cmutex);

//	pxy_conn_ctx_t *parent_ctx = ctx->parent_ctx;
	pxy_conn_ctx_t *parent_ctx = ctx->mctx->parent_ctx;

	int rv = 0;
	if (!ctx) {
		log_dbg_printf(">>>>>??????????????????????????? pxy_bev_writecb_e2: NULL ctx <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< GONE\n");
		goto leave;
	}

	char event_name[6] = "\0\0\0\0\0\0";
	if (bev == ctx->src.bev) {
		strcpy(event_name, "src");
	} else if (bev == ctx->dst.bev) {
		strcpy(event_name, "dst");
	} else if (bev == ctx->e2src.bev) {
		strcpy(event_name, "e2src");
	} else if (bev == ctx->e2dst.bev) {
		strcpy(event_name, "e2dst");
	} else if (bev == NULL) {
		strcpy(event_name, "NULL");
	} else {
		strcpy(event_name, "UNKWN");
	}
	
	log_dbg_printf(">>>>>??????????????????????????? pxy_bev_writecb_e2: %s, %d\n", event_name, ctx->fd);

	evutil_socket_t fd = ctx->fd;

	int src_eof = 1;
	int e2src_eof = 1;
	if (parent_ctx) {
		src_eof = parent_ctx->src_eof;
		e2src_eof = parent_ctx->e2src_eof;
		log_dbg_printf(">>>>>??????????????????????????? pxy_bev_writecb_e2: %s, %d-%d-%d-%d, fd=%d\n", event_name,
				src_eof, e2src_eof, ctx->e2dst_eof, ctx->dst_eof, fd);
	} else {
		log_dbg_printf(">>>>>??????????????????????????? pxy_bev_writecb_e2: ctx->parent_ctx NULL %s, %d\n", event_name, fd);
	}

	if (src_eof || e2src_eof || ctx->e2dst_eof || ctx->dst_eof) {
		log_dbg_printf(">>>>>??????????????????????????? pxy_bev_writecb_e2: 1+ EOF ASSUME e2dst EOF: %d\n", fd);
		rv = pxy_conn_free_e2(ctx);
	}
	
leave:
	log_dbg_printf(">>>>>??????????????????????????? pxy_bev_writecb_e2: EXIT\n");
	if (rv == 2) {
		log_dbg_printf(">>>>>??????????????????????????? pxy_bev_writecb_e2: EXIT FREE META CTX\n");
		pthread_mutex_destroy(cmutex);
		free(mctx);
	} else {
		pthread_mutex_unlock(cmutex);
	}
}

/*
 * Callback for meta events on the up- and downstream connection bufferevents.
 * Called when EOF has been reached, a connection has been made, and on errors.
 */
static void
pxy_bev_eventcb(struct bufferevent *bev, short events, void *arg)
{
	pxy_conn_ctx_t *ctx = arg;
	if (!ctx) {
		log_dbg_printf(">>>>>=================================== pxy_bev_eventcb: ctx NULL\n");
		return;
	}
	if (!ctx->mctx) {
		log_dbg_printf(">>>>>=================================== pxy_bev_eventcb: ctx->mctx NULL\n");
		return;
	}

//	pthread_mutex_t *cmutex = &ctx->thrmgr->mutex2;
	proxy_conn_meta_ctx_t *mctx = ctx->mctx;
	pthread_mutex_t *cmutex = &mctx->mutex;
	pthread_mutex_lock(cmutex);
	
	int rv = 0;

	evutil_socket_t fd = -1;
	if (!ctx) {
		log_dbg_printf(">>>>>=================================== pxy_bev_eventcb: NULL ctx <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< GONE\n");
		goto leave;
	}
	fd = ctx->fd;
	
	log_dbg_printf(">>>>>=================================== pxy_bev_eventcb ENTER fd=%d\n", ctx->fd);

	char event_name[6] = "\0\0\0\0\0\0";
	if (bev == ctx->src.bev) {
		strcpy(event_name, "src");
	} else if (bev == ctx->dst.bev) {
		strcpy(event_name, "dst");
	} else if (bev == ctx->e2src.bev) {
		strcpy(event_name, "e2src");
	} else if (bev == ctx->e2dst.bev) {
		strcpy(event_name, "e2dst");
	} else if (bev == NULL) {
		strcpy(event_name, "NULL");
	} else {
		strcpy(event_name, "UNKWN");
		log_dbg_printf(">>>>>=================================== pxy_bev_eventcb: event_name == UNKWN <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< NOT INIT\n");
		goto leave;
	}

	if (events & BEV_EVENT_CONNECTED) {
		if (!pxy_connected_enable(bev, ctx, event_name)) {
			goto leave;
		}

//		log_dbg_printf(">>>>>=================================== pxy_bev_eventcb: WILL call pxy_srccert_write <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< SSL\n");
		pxy_conn_desc_t *src_ctx = &ctx->src;

		/* write SSL certificates to gendir */
		if (src_ctx->ssl && (bev == ctx->src.bev) && ctx->opts->certgendir) {
			log_dbg_printf(">>>>>=================================== pxy_bev_eventcb: pxy_srccert_write <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< SSL\n");
			pxy_srccert_write(ctx);
		}

		if (OPTS_DEBUG(ctx->opts)) {
			if (src_ctx->ssl) {
				/* for SSL, we get two connect events */
				log_dbg_printf("SSL connected %s [%s]:%s"
				               " %s %s\n",
				               bev == ctx->dst.bev ?
				               "to" : "from",
				               bev == ctx->dst.bev ?
				               ctx->dsthost_str :
				               ctx->srchost_str,
				               bev == ctx->dst.bev ?
				               ctx->dstport_str :
				               ctx->srcport_str,
				               SSL_get_version(src_ctx->ssl),
				               SSL_get_cipher(src_ctx->ssl));
			} else {
				/* for TCP, we get only a dst connect event,
				 * since src was already connected from the
				 * beginning; mirror SSL debug output anyway
				 * in order not to confuse anyone who might be
				 * looking closely at the output */
				log_dbg_printf("TCP connected to [%s]:%s\n",
				               ctx->dsthost_str,
				               ctx->dstport_str);
				log_dbg_printf("TCP connected from [%s]:%s\n",
				               ctx->srchost_str,
				               ctx->srcport_str);
			}
		}

	}

	if (events & BEV_EVENT_ERROR) {
		log_dbg_printf(">>>>>=================================== pxy_bev_eventcb: ERROR %s fd=%d\n", event_name, ctx->fd);

		unsigned long sslerr;
		int have_sslerr = 0;

		/* Can happen for socket errs, ssl errs;
		 * may happen for unclean ssl socket shutdowns. */
		sslerr = bufferevent_get_openssl_error(bev);
		if (sslerr)
			have_sslerr = 1;
		if (!errno && !sslerr) {
#if LIBEVENT_VERSION_NUMBER >= 0x02010000
			/* We have disabled notification for unclean shutdowns
			 * so this should not happen; log a warning. */
			log_err_printf("Warning: Spurious error from "
			               "bufferevent (errno=0,sslerr=0)\n");
#else /* LIBEVENT_VERSION_NUMBER < 0x02010000 */
			/* Older versions of libevent will report these. */
			if (OPTS_DEBUG(ctx->opts)) {
				log_dbg_printf("Unclean SSL shutdown.\n");
			}
#endif /* LIBEVENT_VERSION_NUMBER < 0x02010000 */
		} else if (ERR_GET_REASON(sslerr) ==
		           SSL_R_SSLV3_ALERT_HANDSHAKE_FAILURE) {
			/* these can happen due to client cert auth,
			 * only log error if debugging is activated */
			log_dbg_printf("Error from bufferevent: "
			               "%i:%s %lu:%i:%s:%i:%s:%i:%s\n",
			               errno,
			               errno ? strerror(errno) : "-",
			               sslerr,
			               ERR_GET_REASON(sslerr),
			               sslerr ?
			               ERR_reason_error_string(sslerr) : "-",
			               ERR_GET_LIB(sslerr),
			               sslerr ?
			               ERR_lib_error_string(sslerr) : "-",
			               ERR_GET_FUNC(sslerr),
			               sslerr ?
			               ERR_func_error_string(sslerr) : "-");
			while ((sslerr = bufferevent_get_openssl_error(bev))) {
				log_dbg_printf("Additional SSL error: "
				               "%lu:%i:%s:%i:%s:%i:%s\n",
				               sslerr,
				               ERR_GET_REASON(sslerr),
				               ERR_reason_error_string(sslerr),
				               ERR_GET_LIB(sslerr),
				               ERR_lib_error_string(sslerr),
				               ERR_GET_FUNC(sslerr),
				               ERR_func_error_string(sslerr));
			}
		} else {
			/* real errors */
			log_err_printf("Error from bufferevent: "
			               "%i:%s %lu:%i:%s:%i:%s:%i:%s\n",
			               errno,
			               errno ? strerror(errno) : "-",
			               sslerr,
			               ERR_GET_REASON(sslerr),
			               sslerr ?
			               ERR_reason_error_string(sslerr) : "-",
			               ERR_GET_LIB(sslerr),
			               sslerr ?
			               ERR_lib_error_string(sslerr) : "-",
			               ERR_GET_FUNC(sslerr),
			               sslerr ?
			               ERR_func_error_string(sslerr) : "-");
			while ((sslerr = bufferevent_get_openssl_error(bev))) {
				log_err_printf("Additional SSL error: "
				               "%lu:%i:%s:%i:%s:%i:%s\n",
				               sslerr,
				               ERR_GET_REASON(sslerr),
				               ERR_reason_error_string(sslerr),
				               ERR_GET_LIB(sslerr),
				               ERR_lib_error_string(sslerr),
				               ERR_GET_FUNC(sslerr),
				               ERR_func_error_string(sslerr));
			}
		}

		pxy_conn_desc_t *src_ctx = &ctx->src;
		/* we only get a single disconnect event here for both connections */
		if (OPTS_DEBUG(ctx->opts)) {
			log_dbg_printf("%s disconnected to [%s]:%s\n",
						   src_ctx->ssl ? "SSL" : "TCP",
						   ctx->dsthost_str, ctx->dstport_str);
			log_dbg_printf("%s disconnected from [%s]:%s\n",
						   src_ctx->ssl ? "SSL" : "TCP",
						   ctx->srchost_str, ctx->srcport_str);
		}

//		this->closed = 1;
//		bufferevent_free_and_close_fd(bev, ctx);
//		this->bev = NULL;
//		if (other->closed) {
//			pxy_conn_ctx_free(ctx);
//		}
	}

	if (events & BEV_EVENT_EOF) {
		if (bev == ctx->dst.bev) {
			log_dbg_printf(">>>>>=================================== pxy_bev_eventcb: dst EOF: %d\n", ctx->fd);
			ctx->dst_eof = 1;
		} else if (bev == ctx->e2src.bev) {
			log_dbg_printf(">>>>>=================================== pxy_bev_eventcb: e2src EOF: %d\n", ctx->fd);
			ctx->e2src_eof = 1;
		} else if (bev == ctx->src.bev) {
			log_dbg_printf(">>>>>=================================== pxy_bev_eventcb: src EOF: %d\n", ctx->fd);
			ctx->src_eof = 1;
		}
		log_dbg_printf(">>>>>=================================== pxy_bev_eventcb: EOF %s, %d-%d-%d-%d, fd=%d\n", event_name,
				ctx->src_eof, ctx->e2src_eof, ctx->e2dst_eof, ctx->dst_eof, ctx->fd);
	}

	if (ctx->src_eof || ctx->e2src_eof) {
		log_dbg_printf(">>>>>=================================== pxy_bev_eventcb(): 1+ EOF TRY FREEING fd=%d\n", ctx->fd);
		rv = pxy_conn_free(ctx);
		goto leave;
	}
		
leave:
	log_dbg_printf(">>>>>=================================== pxy_bev_eventcb EXIT fd=%d\n", fd);

	if (rv == 2) {
		log_dbg_printf(">>>>>=================================== pxy_bev_eventcb: EXIT FREE META CTX\n");
		pthread_mutex_destroy(cmutex);
		free(mctx);
	} else {
		pthread_mutex_unlock(cmutex);
	}
}

static void
pxy_bev_eventcb_e2(struct bufferevent *bev, short events, void *arg)
{
	pxy_conn_ctx_t *ctx = arg;
	if (!ctx) {
		log_dbg_printf(">>>>>--------------------- pxy_bev_eventcb_e2: ctx NULL\n");
		return;
	}
	if (!ctx->mctx) {
		log_dbg_printf(">>>>>--------------------- pxy_bev_eventcb_e2: ctx->mctx NULL\n");
		return;
	}

//	pthread_mutex_t *cmutex = &ctx->thrmgr->mutex2;
	proxy_conn_meta_ctx_t *mctx = ctx->mctx;
	pthread_mutex_t *cmutex = &mctx->mutex;
	pthread_mutex_lock(cmutex);

	int rv = 0;

	if (!ctx) {
		log_dbg_printf(">>>>>--------------------- pxy_bev_eventcb_e2: NULL ctx <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< GONE\n");
		goto leave;
	}

//	pxy_conn_ctx_t *parent_ctx = ctx->parent_ctx;
	pxy_conn_ctx_t *parent_ctx = ctx->mctx->parent_ctx;

	char event_name[6] = "\0\0\0\0\0\0";
	if (bev == ctx->src.bev) {
		strcpy(event_name, "src");
	} else if (bev == ctx->dst.bev) {
		strcpy(event_name, "dst");
	} else if (bev == ctx->e2src.bev) {
		strcpy(event_name, "e2src");
	} else if (bev == ctx->e2dst.bev) {
		strcpy(event_name, "e2dst");
	} else if (bev == NULL) {
		strcpy(event_name, "NULL");
	} else {
		strcpy(event_name, "UNKWN");
	}

	log_dbg_printf(">>>>>--------------------- pxy_bev_eventcb_e2: ENTER %s fd=%d\n", event_name, ctx->fd);

	if (events & BEV_EVENT_CONNECTED) {
		log_dbg_printf(">>>>>--------------------- pxy_bev_eventcb_e2: CONNECTED %s fd=%d\n", event_name, ctx->fd);
	}

	int fd = ctx->fd;
	
	if (events & BEV_EVENT_EOF) {
		int e2dst_eof = ctx->e2dst_eof;
		int dst_eof = ctx->dst_eof;
		
		if (bev == ctx->e2dst.bev) {
			log_dbg_printf(">>>>>--------------------- pxy_bev_eventcb_e2: e2dst EOF: %d\n", fd);

			ctx->e2dst_eof = 1;

			rv = pxy_conn_free_e2(ctx);
		}
		else if (bev == ctx->dst.bev) {
			log_dbg_printf(">>>>>--------------------- pxy_bev_eventcb_e2: dst EOF: %d\n", fd);

			ctx->dst_eof = 1;

			rv = pxy_conn_free_e2(ctx);
		}
		if (parent_ctx) {
			int src_eof = parent_ctx->src_eof;
			int e2src_eof = parent_ctx->e2src_eof;
			log_dbg_printf(">>>>>--------------------- pxy_bev_eventcb_e2: EOF %s, %d-%d-%d-%d, fd=%d\n", event_name,
				src_eof, e2src_eof, e2dst_eof, dst_eof, fd);
		} else {
			log_dbg_printf(">>>>>--------------------- pxy_bev_eventcb_e2: EOF %s, NO PARENT, %d-%d, fd=%d\n", event_name,
				e2dst_eof, dst_eof, fd);
		}
	}

	if (events & BEV_EVENT_ERROR) {
		log_dbg_printf(">>>>>--------------------- pxy_bev_eventcb_e2: ERROR\n");
		unsigned long sslerr;
		int have_sslerr = 0;

		/* Can happen for socket errs, ssl errs;
		 * may happen for unclean ssl socket shutdowns. */
		sslerr = bufferevent_get_openssl_error(bev);
		if (sslerr)
			have_sslerr = 1;
		if (!errno && !sslerr) {
#if LIBEVENT_VERSION_NUMBER >= 0x02010000
			/* We have disabled notification for unclean shutdowns
			 * so this should not happen; log a warning. */
			log_err_printf("Warning: Spurious error from "
			               "bufferevent (errno=0,sslerr=0)\n");
#else /* LIBEVENT_VERSION_NUMBER < 0x02010000 */
			/* Older versions of libevent will report these. */
			if (OPTS_DEBUG(ctx->opts)) {
				log_dbg_printf("Unclean SSL shutdown.\n");
			}
#endif /* LIBEVENT_VERSION_NUMBER < 0x02010000 */
		} else if (ERR_GET_REASON(sslerr) ==
		           SSL_R_SSLV3_ALERT_HANDSHAKE_FAILURE) {
			/* these can happen due to client cert auth,
			 * only log error if debugging is activated */
			log_dbg_printf("Error from bufferevent: "
			               "%i:%s %lu:%i:%s:%i:%s:%i:%s\n",
			               errno,
			               errno ? strerror(errno) : "-",
			               sslerr,
			               ERR_GET_REASON(sslerr),
			               sslerr ?
			               ERR_reason_error_string(sslerr) : "-",
			               ERR_GET_LIB(sslerr),
			               sslerr ?
			               ERR_lib_error_string(sslerr) : "-",
			               ERR_GET_FUNC(sslerr),
			               sslerr ?
			               ERR_func_error_string(sslerr) : "-");
			while ((sslerr = bufferevent_get_openssl_error(bev))) {
				log_dbg_printf("Additional SSL error: "
				               "%lu:%i:%s:%i:%s:%i:%s\n",
				               sslerr,
				               ERR_GET_REASON(sslerr),
				               ERR_reason_error_string(sslerr),
				               ERR_GET_LIB(sslerr),
				               ERR_lib_error_string(sslerr),
				               ERR_GET_FUNC(sslerr),
				               ERR_func_error_string(sslerr));
			}
		} else {
			/* real errors */
			log_err_printf("Error from bufferevent: "
			               "%i:%s %lu:%i:%s:%i:%s:%i:%s\n",
			               errno,
			               errno ? strerror(errno) : "-",
			               sslerr,
			               ERR_GET_REASON(sslerr),
			               sslerr ?
			               ERR_reason_error_string(sslerr) : "-",
			               ERR_GET_LIB(sslerr),
			               sslerr ?
			               ERR_lib_error_string(sslerr) : "-",
			               ERR_GET_FUNC(sslerr),
			               sslerr ?
			               ERR_func_error_string(sslerr) : "-");
			while ((sslerr = bufferevent_get_openssl_error(bev))) {
				log_err_printf("Additional SSL error: "
				               "%lu:%i:%s:%i:%s:%i:%s\n",
				               sslerr,
				               ERR_GET_REASON(sslerr),
				               ERR_reason_error_string(sslerr),
				               ERR_GET_LIB(sslerr),
				               ERR_lib_error_string(sslerr),
				               ERR_GET_FUNC(sslerr),
				               ERR_func_error_string(sslerr));
			}
		}

	}

leave:
	log_dbg_printf(">>>>>--------------------- pxy_bev_eventcb_e2: EXIT\n");

	if (rv == 2) {
		log_dbg_printf(">>>>>--------------------- pxy_bev_eventcb_e2: EXIT FREE META CTX\n");
		pthread_mutex_destroy(cmutex);
		free(mctx);
	} else {
		pthread_mutex_unlock(cmutex);
	}
}

/*
 * Complete the connection.  This gets called after finding out where to
 * connect to.
 */
static void
pxy_conn_connect(pxy_conn_ctx_t *ctx)
{
	log_dbg_printf(">>>>> pxy_conn_connect: ENTER fd=%d\n", ctx->fd);
	if (!ctx->addrlen) {
		log_err_printf("No target address; aborting connection\n");
		evutil_closesocket(ctx->fd);
		pxy_conn_ctx_free(ctx);
		return;
	}

	/* create server-side socket and eventbuffer */
	if (ctx->spec->ssl && !ctx->passthrough) {
		ctx->dst.ssl = pxy_dstssl_create(ctx);
		if (!ctx->dst.ssl) {
			log_err_printf("Error creating SSL\n");
			evutil_closesocket(ctx->fd);
			pxy_conn_ctx_free(ctx);
			return;
		}
	}

	log_dbg_printf(">>>>> pxy_conn_connect: pxy_bufferevent_setup for dst fd=%d\n", ctx->fd);
	ctx->dst.bev = pxy_bufferevent_setup(ctx, -1, ctx->dst.ssl);
	if (!ctx->dst.bev) {
		if (ctx->dst.ssl) {
			SSL_free(ctx->dst.ssl);
			ctx->dst.ssl = NULL;
		}
		evutil_closesocket(ctx->fd);
		pxy_conn_ctx_free(ctx);
		return;
	}
	log_dbg_printf(">>>>> pxy_conn_connect: <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< bufferevent_enable(ctx->dst.bev)\n");
	bufferevent_enable(ctx->dst.bev, EV_READ|EV_WRITE);

	if (OPTS_DEBUG(ctx->opts)) {
		char *host, *port;
		if (sys_sockaddr_str((struct sockaddr *)&ctx->addr,
		                     ctx->addrlen, &host, &port) != 0) {
			log_dbg_printf("Connecting to [?]:?\n");
		} else {
			log_dbg_printf("Connecting to [%s]:%s\n", host, port);
			free(host);
			free(port);
		}
	}

	/* initiate connection */
	log_dbg_printf(">>>>> pxy_conn_connect: bufferevent_socket_connect for dst fd=%d\n", ctx->fd);
	bufferevent_socket_connect(ctx->dst.bev,
	                           (struct sockaddr *)&ctx->addr,
	                           ctx->addrlen);

	log_dbg_printf(">>>>> pxy_conn_connect: EXIT fd=%d\n", ctx->fd);
}

#ifndef OPENSSL_NO_TLSEXT
/*
 * The SNI hostname has been resolved.  Fill the first resolved address into
 * the context and continue connecting.
 */
static void
pxy_sni_resolve_cb(int errcode, struct evutil_addrinfo *ai, void *arg)
{
	pxy_conn_ctx_t *ctx = arg;

	if (errcode) {
		log_err_printf("Cannot resolve SNI hostname '%s': %s\n",
		               ctx->sni, evutil_gai_strerror(errcode));
		evutil_closesocket(ctx->fd);
		pxy_conn_ctx_free(ctx);
		return;
	}

	memcpy(&ctx->addr, ai->ai_addr, ai->ai_addrlen);
	ctx->addrlen = ai->ai_addrlen;
	evutil_freeaddrinfo(ai);
	pxy_conn_connect(ctx);
}
#endif /* !OPENSSL_NO_TLSEXT */

/*
 * The src fd is readable.  This is used to sneak-preview the SNI on SSL
 * connections.  If ctx->ev is NULL, it was called manually for a non-SSL
 * connection.  If ctx->passthrough is set, it was called a second time
 * after the first ssl callout failed because of client cert auth.
 */
#ifndef OPENSSL_NO_TLSEXT
#define MAYBE_UNUSED 
#else /* OPENSSL_NO_TLSEXT */
#define MAYBE_UNUSED UNUSED
#endif /* OPENSSL_NO_TLSEXT */
static void
pxy_fd_readcb(MAYBE_UNUSED evutil_socket_t fd, UNUSED short what, void *arg)
#undef MAYBE_UNUSED
{
	pxy_conn_ctx_t *ctx = arg;

#ifndef OPENSSL_NO_TLSEXT
	/* for SSL, peek ClientHello and parse SNI from it */
	if (ctx->spec->ssl && !ctx->passthrough /*&& ctx->ev*/) {
		unsigned char buf[1024];
		ssize_t n;
		const unsigned char *chello;
		int rv;

		n = recv(fd, buf, sizeof(buf), MSG_PEEK);
		if (n == -1) {
			log_err_printf("Error peeking on fd, aborting "
			               "connection\n");
			evutil_closesocket(fd);
			pxy_conn_ctx_free(ctx);
			return;
		}
		if (n == 0) {
			/* socket got closed while we were waiting */
			evutil_closesocket(fd);
			pxy_conn_ctx_free(ctx);
			return;
		}

		rv = ssl_tls_clienthello_parse(buf, n, 0, &chello, &ctx->sni);
		if ((rv == 1) && !chello) {
			log_err_printf("Peeking did not yield a (truncated) "
			               "ClientHello message, "
			               "aborting connection\n");
			evutil_closesocket(fd);
			pxy_conn_ctx_free(ctx);
			return;
		}
		if (OPTS_DEBUG(ctx->opts)) {
			log_dbg_printf("SNI peek: [%s] [%s]\n",
			               ctx->sni ? ctx->sni : "n/a",
			               ((rv == 1) && chello) ?
			               "incomplete" : "complete");
		}
		if ((rv == 1) && chello && (ctx->sni_peek_retries++ < 50)) {
			/* ssl_tls_clienthello_parse indicates that we
			 * should retry later when we have more data, and we
			 * haven't reached the maximum retry count yet.
			 * Reschedule this event as timeout-only event in
			 * order to prevent busy looping over the read event.
			 * Because we only peeked at the pending bytes and
			 * never actually read them, fd is still ready for
			 * reading now.  We use 25 * 0.2 s = 5 s timeout. */
			struct timeval retry_delay = {0, 100};

			event_free(ctx->ev);
			ctx->ev = event_new(ctx->evbase, fd, 0,
			                    pxy_fd_readcb, ctx);
			if (!ctx->ev) {
				log_err_printf("Error creating retry "
				               "event, aborting "
				               "connection\n");
				evutil_closesocket(fd);
				pxy_conn_ctx_free(ctx);
				return;
			}
			event_add(ctx->ev, &retry_delay);
			return;
		}
		event_free(ctx->ev);
		ctx->ev = NULL;
	}

	if (ctx->sni && !ctx->addrlen && ctx->spec->sni_port) {
		char sniport[6];
		struct evutil_addrinfo hints;

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = ctx->af;
		hints.ai_flags = EVUTIL_AI_ADDRCONFIG;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		snprintf(sniport, sizeof(sniport), "%i", ctx->spec->sni_port);
		evdns_getaddrinfo(ctx->dnsbase, ctx->sni, sniport, &hints,
		                  pxy_sni_resolve_cb, ctx);
		return;
	}
#endif /* !OPENSSL_NO_TLSEXT */

	log_dbg_printf(">>>>> pxy_fd_readcb() pxy_conn_connect\n");
	pxy_conn_connect(ctx);
	log_dbg_printf(">>>>> EXIT pxy_fd_readcb()\n");
}

/*
 * Callback for accept events on the socket listener bufferevent.
 * Called when a new incoming connection has been accepted.
 * Initiates the connection to the server.  The incoming connection
 * from the client is not being activated until we have a successful
 * connection to the server, because we need the server's certificate
 * in order to set up the SSL session to the client.
 * For consistency, plain TCP works the same way, even if we could
 * start reading from the client while waiting on the connection to
 * the server to connect.
 */
pxy_conn_ctx_t *
pxy_conn_setup(evutil_socket_t fd,
               struct sockaddr *peeraddr, int peeraddrlen,
               proxy_conn_meta_ctx_t *mctx, int fd2)
//               pxy_thrmgr_ctx_t *thrmgr,
//               proxyspec_t *spec, opts_t *opts, int fd2)
{
	log_dbg_printf(">>>>> pxy_conn_setup(): fd=%d\n", fd);

	pxy_thrmgr_ctx_t *thrmgr = mctx->lctx->thrmgr;
	proxyspec_t *spec = mctx->lctx->spec;
	opts_t *opts = mctx->lctx->opts;

	/* create per connection pair state and attach to thread */
	pxy_conn_ctx_t *ctx = pxy_conn_ctx_new(spec, opts, thrmgr, fd);
	if (!ctx) {
		log_err_printf("Error allocating memory\n");
		evutil_closesocket(fd);
		return NULL;
	}
	
	ctx->mctx = mctx;
	// Not to close the connection during initialization
	ctx->initialized = 0;

	ctx->af = peeraddr->sa_family;

	/* determine original destination of connection */
	if (spec->natlookup) {
		/* NAT engine lookup */
		ctx->addrlen = sizeof(struct sockaddr_storage);
		log_dbg_printf(">>>>> pxy_conn_setup() natlookup\n");
		if (spec->natlookup((struct sockaddr *)&ctx->addr, &ctx->addrlen,
		                    fd, peeraddr, peeraddrlen) == -1) {
			char *cbuf = NULL;
			char *chbuf, *cpbuf;
			
			sys_sockipport_str(peeraddr, peeraddrlen, &chbuf, &cpbuf);
			if (asprintf(&cbuf, "\naddr= [%s]:%s", chbuf, cpbuf) < 0) {
				return NULL;
			}

			log_err_printf("Connection not found in NAT "
			               "state table, aborting connection: %s\n", cbuf);

			evutil_closesocket(fd);
			pxy_conn_ctx_free(ctx);
			return NULL;
		}
	} else if (spec->connect_addrlen > 0) {
		/* static forwarding */
		ctx->addrlen = spec->connect_addrlen;
		memcpy(&ctx->addr, &spec->connect_addr, ctx->addrlen);
	} else {
		/* SNI mode */
		if (!ctx->spec->ssl) {
			/* if this happens, the proxyspec parser is broken */
			log_err_printf("SNI mode used for non-SSL connection; "
			               "aborting connection\n");
			evutil_closesocket(fd);
			pxy_conn_ctx_free(ctx);
			return NULL;
		}
	}

//	log_dbg_printf(">>>>> pxy_conn_setup() pxy_fd_readcb\n");
//	pxy_fd_readcb(fd, 0, ctx);
	/* for SSL, defer dst connection setup to initial_readcb */
	if (ctx->spec->ssl) {
		ctx->ev = event_new(ctx->evbase, fd, EV_READ, pxy_fd_readcb, ctx);
//		if (!ctx->ev)
//			goto memout;
		event_add(ctx->ev, NULL);
	} else {
		log_dbg_printf(">>>>> pxy_conn_setup() pxy_fd_readcb\n");
		pxy_fd_readcb(fd, 0, ctx);
	}
	
	if (!ctx) {
		log_dbg_printf(">>>>> pxy_conn_setup: NULL CTX\n");
	}
	
	log_dbg_printf(">>>>> EXIT pxy_conn_setup()\n");
	return ctx;
}

void
pxy_conn_setup_e2(evutil_socket_t fd,
//               struct sockaddr *peeraddr, int peeraddrlen,
               proxy_conn_meta_ctx_t *mctx)
//               pxy_thrmgr_ctx_t *thrmgr,
//               proxyspec_t *spec, opts_t *opts, proxy_conn_meta_ctx_t *mctx)
{
	log_dbg_printf(">>>>> pxy_conn_setup_e2: ENTER fd=%d\n", fd);

	pxy_thrmgr_ctx_t *thrmgr = mctx->lctx->thrmgr;
	proxyspec_t *spec = mctx->lctx->spec;
	opts_t *opts = mctx->lctx->opts;

	pxy_conn_ctx_t *parent_ctx = mctx->parent_ctx;
	int pfd = -1;
	if (!parent_ctx) {
		log_dbg_printf(">>>>> pxy_conn_setup_e2: NULL parent_ctx <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< GONE\n");
		goto leave;
	}
		
	pfd = parent_ctx->fd;

	pxy_conn_ctx_t *ctx = pxy_conn_ctx_new(spec, opts, thrmgr, fd);
	if (!ctx) {
		log_err_printf("Error allocating memory\n");
		evutil_closesocket(fd);
		goto leave;
	}

	ctx->mctx = mctx;
	ctx->child_ctx = NULL;

//	log_dbg_printf(">>>>> pxy_conn_setup_e2: pxy_bufferevent_setup_e2 for dst.bev, fd=%d\n", fd);
//	ctx->dst.bev = pxy_bufferevent_setup_e2(ctx, -1, parent_ctx->dst.ssl);

	if (ctx->spec->ssl && !ctx->passthrough) {
		ctx->dst.ssl = pxy_dstssl_create(ctx);
		if (!ctx->dst.ssl) {
			log_dbg_printf(">>>>> pxy_conn_setup_e2: ctx->dst.ssl NULL, fd=%d\n", fd);
//			log_err_printf("Error creating SSL\n");
//			evutil_closesocket(ctx->fd);
//			pxy_conn_ctx_free(ctx);
//			return;
		}
	}

//	log_dbg_printf(">>>>> pxy_conn_connect: pxy_bufferevent_setup for dst fd=%d\n", ctx->fd);
//	ctx->dst.bev = pxy_bufferevent_setup(ctx, -1, ctx->dst.ssl);
	log_dbg_printf(">>>>> pxy_conn_setup_e2: pxy_bufferevent_setup_e2 for dst.bev, fd=%d\n", fd);
	ctx->dst.bev = pxy_bufferevent_setup_e2(ctx, -1, ctx->dst.ssl);
	if (!ctx->dst.bev) {
		if (ctx->dst.ssl) {
			SSL_free(ctx->dst.ssl);
			ctx->dst.ssl = NULL;
		}
		evutil_closesocket(ctx->fd);
		pxy_conn_ctx_free(ctx);
		return;
	}
	log_dbg_printf(">>>>> pxy_conn_setup_e2: <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< bufferevent_enable(ctx->dst.bev)\n");
	bufferevent_enable(ctx->dst.bev, EV_READ|EV_WRITE);
	
	/* initiate connection */
	log_dbg_printf(">>>>> pxy_conn_setup_e2: bufferevent_socket_connect dst.bev\n");
	bufferevent_socket_connect(ctx->dst.bev, (struct sockaddr *)&parent_ctx->addr, parent_ctx->addrlen);

	log_dbg_printf(">>>>> pxy_conn_setup_e2: pxy_bufferevent_setup_e2 for e2dst.bev, fd=%d\n", fd);
	ctx->e2dst.bev = pxy_bufferevent_setup_e2(ctx, fd, ctx->e2dst.ssl);

	if (!parent_ctx) {
		log_dbg_printf(">>>>> pxy_conn_setup_e2: parent_ctx NULL >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> fd=%d\n", fd);
	} else if (!mctx->child_ctx) {
		log_dbg_printf(">>>>> pxy_conn_setup_e2: parent_ctx->child_ctx NULL >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> fd=%d\n", fd);
	}

	// Handle first child, if the last child is deleted, the child_ctx becomes null again
	if (!parent_ctx->initialized) {
		log_dbg_printf(">>>>> pxy_conn_setup_e2: parent_ctx->e2dst FIRST CHILD >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> INITIALIZED\n");
		parent_ctx->initialized = 1;
	} else {
		if (mctx->child_ctx) {
			log_dbg_printf(">>>>> pxy_conn_setup_e2: parent_ctx->e2dst NEW CHILD >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> prev CHILD EXISTS\n");
		} else {
			log_dbg_printf(">>>>> pxy_conn_setup_e2: parent_ctx->e2dst NEW CHILD >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> fd=%d, NO PREV CHILD\n", fd);
		}

		ctx->child_ctx = mctx->child_ctx;
	}

	mctx->child_ctx = ctx;

	log_dbg_printf(">>>>> pxy_conn_setup_e2: set callbacks for dst.bev\n");
	bufferevent_setcb(ctx->dst.bev, pxy_bev_readcb_e2, pxy_bev_writecb_e2, pxy_bev_eventcb_e2, ctx);
//	bufferevent_enable(ctx->dst.bev, EV_READ|EV_WRITE);

	log_dbg_printf(">>>>> pxy_conn_setup_e2: set callbacks for e2dst.bev\n");
	bufferevent_setcb(ctx->e2dst.bev, pxy_bev_readcb_e2, pxy_bev_writecb_e2, pxy_bev_eventcb_e2, ctx);

	// @todo Do we need a watermark?
	//bufferevent_setwatermark(ctx->e2dst.bev, EV_READ, 200, OUTBUF_LIMIT);

	log_dbg_printf(">>>>> pxy_conn_setup_e2: enable callbacks\n");
	bufferevent_enable(ctx->dst.bev, EV_READ|EV_WRITE);
	bufferevent_enable(ctx->e2dst.bev, EV_READ|EV_WRITE);

leave:
	log_dbg_printf(">>>>> pxy_conn_setup_e2(): EXIT fd=%d, parent fd=%d\n", fd, pfd);
}
/* vim: set noet ft=c: */
