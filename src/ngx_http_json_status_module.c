/*
 * Copyright (c) 2026, Jochen Neumeister <joneum@FreeBSD.org>
 *
 * Use of this source code is governed by the BSD 2-Clause license that can
 * be found in the LICENSE file.
 */


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>


#if !(NGX_STAT_STUB)
#error "the counters live behind NGX_STAT_STUB; the module's config sets it"
#endif


/*
 * The answer is written into a chain of buffers that grows as it is filled,
 * not into one buffer whose size was guessed beforehand.  Guessing would
 * mean walking the peer list twice, once to measure and once to write, and
 * between the two the list can change -- a peer can come or go with the
 * resolver.  A measurement that is already stale when it is used is how a
 * status page overflows its own buffer.
 */

#define NGX_HTTP_JSON_STATUS_CHUNK  2048


typedef struct {
    ngx_http_request_t  *r;
    ngx_chain_t         *out;
    ngx_chain_t        **last;
    ngx_buf_t           *buf;
    off_t                len;
} ngx_http_json_status_writer_t;


static u_char *ngx_http_json_status_reserve(
    ngx_http_json_status_writer_t *w, size_t need);
static void ngx_http_json_status_commit(ngx_http_json_status_writer_t *w,
    u_char *p);
static u_char *ngx_http_json_status_string(u_char *p, ngx_str_t *s);
static ngx_int_t ngx_http_json_status_upstreams(
    ngx_http_json_status_writer_t *w);
static ngx_int_t ngx_http_json_status_peers(ngx_http_json_status_writer_t *w,
    ngx_http_upstream_rr_peers_t *peers, ngx_uint_t backup, ngx_uint_t *first);
static ngx_int_t ngx_http_json_status_handler(ngx_http_request_t *r);
static char *ngx_http_json_status(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf);


static ngx_command_t  ngx_http_json_status_commands[] = {

    { ngx_string("json_status"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_json_status,
      0,
      0,
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_json_status_module_ctx = {
    NULL,                                  /* preconfiguration */
    NULL,                                  /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    NULL,                                  /* create location configuration */
    NULL                                   /* merge location configuration */
};


ngx_module_t  ngx_http_json_status_module = {
    NGX_MODULE_V1,
    &ngx_http_json_status_module_ctx,      /* module context */
    ngx_http_json_status_commands,         /* module directives */
    NGX_HTTP_MODULE,                       /* module type */
    NULL,                                  /* init master */
    NULL,                                  /* init module */
    NULL,                                  /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    NULL,                                  /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


/*
 * Hands back a pointer with at least "need" bytes of room.  A chunk that is
 * too small for a single item is allocated at the size the item needs, so
 * there is no upper limit on the length of a server name.
 */

static u_char *
ngx_http_json_status_reserve(ngx_http_json_status_writer_t *w, size_t need)
{
    size_t        size;
    ngx_buf_t    *b;
    ngx_chain_t  *cl;

    if (w->buf != NULL && (size_t) (w->buf->end - w->buf->last) >= need) {
        return w->buf->last;
    }

    size = need > NGX_HTTP_JSON_STATUS_CHUNK
           ? need : NGX_HTTP_JSON_STATUS_CHUNK;

    b = ngx_create_temp_buf(w->r->pool, size);
    if (b == NULL) {
        return NULL;
    }

    b->memory = 1;

    cl = ngx_alloc_chain_link(w->r->pool);
    if (cl == NULL) {
        return NULL;
    }

    cl->buf = b;
    cl->next = NULL;

    *w->last = cl;
    w->last = &cl->next;
    w->buf = b;

    return b->last;
}


static void
ngx_http_json_status_commit(ngx_http_json_status_writer_t *w, u_char *p)
{
    w->len += p - w->buf->last;
    w->buf->last = p;
}


/*
 * A JSON string.  Upstream names and server addresses come out of the
 * configuration, so a quote or a backslash in them is unlikely -- but a
 * status page that can be made to emit broken JSON by a configuration is
 * not a status page.  Worst case six bytes per input byte, which is what
 * the callers reserve.
 */

static u_char *
ngx_http_json_status_string(u_char *p, ngx_str_t *s)
{
    u_char  *d, *last;

    *p++ = '"';

    last = s->data + s->len;

    for (d = s->data; d < last; d++) {

        switch (*d) {

        case '"':
        case '\\':
            *p++ = '\\';
            *p++ = *d;
            break;

        case '\n':
            *p++ = '\\';
            *p++ = 'n';
            break;

        case '\r':
            *p++ = '\\';
            *p++ = 'r';
            break;

        case '\t':
            *p++ = '\\';
            *p++ = 't';
            break;

        default:
            if (*d < 0x20 || *d == 0x7f) {
                p = ngx_sprintf(p, "\\u%04Xi", (ngx_int_t) *d);
            } else {
                *p++ = *d;
            }
        }
    }

    *p++ = '"';

    return p;
}


static ngx_int_t
ngx_http_json_status_peers(ngx_http_json_status_writer_t *w,
    ngx_http_upstream_rr_peers_t *peers, ngx_uint_t backup, ngx_uint_t *first)
{
    u_char                       *p;
    size_t                        need;
    ngx_http_upstream_rr_peer_t  *peer;

    for (peer = peers->peer; peer; peer = peer->next) {

        need = sizeof("{\"server\":,\"name\":,\"weight\":,\"conns\":,"
                      "\"max_conns\":,\"fails\":,\"max_fails\":,"
                      "\"fail_timeout\":,\"down\":false,\"backup\":false},")
               + 2 + 6 * peer->server.len
               + 2 + 6 * peer->name.len
               + 6 * NGX_INT_T_LEN;

        p = ngx_http_json_status_reserve(w, need);
        if (p == NULL) {
            return NGX_ERROR;
        }

        if (!*first) {
            *p++ = ',';
        }
        *first = 0;

        p = ngx_cpymem(p, "{\"server\":", sizeof("{\"server\":") - 1);
        p = ngx_http_json_status_string(p, &peer->server);

        p = ngx_cpymem(p, ",\"name\":", sizeof(",\"name\":") - 1);
        p = ngx_http_json_status_string(p, &peer->name);

        p = ngx_sprintf(p, ",\"weight\":%i", peer->weight);
        p = ngx_sprintf(p, ",\"conns\":%ui", peer->conns);
        p = ngx_sprintf(p, ",\"max_conns\":%ui", peer->max_conns);
        p = ngx_sprintf(p, ",\"fails\":%ui", peer->fails);
        p = ngx_sprintf(p, ",\"max_fails\":%ui", peer->max_fails);
        p = ngx_sprintf(p, ",\"fail_timeout\":%T", peer->fail_timeout);

        p = ngx_cpymem(p, ",\"down\":", sizeof(",\"down\":") - 1);
        p = peer->down ? ngx_cpymem(p, "true", 4) : ngx_cpymem(p, "false", 5);

        p = ngx_cpymem(p, ",\"backup\":", sizeof(",\"backup\":") - 1);
        p = backup ? ngx_cpymem(p, "true", 4) : ngx_cpymem(p, "false", 5);

        *p++ = '}';

        ngx_http_json_status_commit(w, p);
    }

    return NGX_OK;
}


/*
 * Per-server numbers are only reported for an upstream that has a "zone".
 * Without one, us->peer.data is whatever the configured balancer put there,
 * and reading it as a round robin peer list is a guess.  With a zone, the
 * zone module itself builds that list in shared memory, so the read is
 * sound -- and the numbers are the only ones worth printing anyway, because
 * without shared memory every worker counts for itself.
 */

static ngx_int_t
ngx_http_json_status_upstreams(ngx_http_json_status_writer_t *w)
{
    u_char                        *p;
    size_t                         need;
    ngx_uint_t                     i, first, pfirst;
    ngx_http_upstream_srv_conf_t  **uscfp, *uscf;
    ngx_http_upstream_rr_peers_t   *peers;
    ngx_http_upstream_main_conf_t  *umcf;

    umcf = ngx_http_get_module_main_conf(w->r, ngx_http_upstream_module);
    uscfp = umcf->upstreams.elts;

    first = 1;

    for (i = 0; i < umcf->upstreams.nelts; i++) {
        uscf = uscfp[i];

        /* proxy_pass with a literal host registers an upstream of its own,
         * named after the host -- and it does have a server, so counting
         * them does not tell the two apart.  NGX_HTTP_UPSTREAM_CREATE does:
         * only the "upstream" directive sets it. */

        if (!(uscf->flags & NGX_HTTP_UPSTREAM_CREATE)) {
            continue;
        }

        need = sizeof("{\"zone\":false,\"peers\":[]},") + 2
               + 6 * uscf->host.len;

        p = ngx_http_json_status_reserve(w, need);
        if (p == NULL) {
            return NGX_ERROR;
        }

        if (!first) {
            *p++ = ',';
        }
        first = 0;

        p = ngx_http_json_status_string(p, &uscf->host);

        if (uscf->shm_zone == NULL || uscf->peer.data == NULL) {
            p = ngx_cpymem(p, ":{\"zone\":false,\"peers\":[]}",
                           sizeof(":{\"zone\":false,\"peers\":[]}") - 1);
            ngx_http_json_status_commit(w, p);
            continue;
        }

        p = ngx_cpymem(p, ":{\"zone\":true,\"peers\":[",
                       sizeof(":{\"zone\":true,\"peers\":[") - 1);
        ngx_http_json_status_commit(w, p);

        peers = uscf->peer.data;
        pfirst = 1;

        ngx_http_upstream_rr_peers_rlock(peers);

        if (ngx_http_json_status_peers(w, peers, 0, &pfirst) != NGX_OK) {
            ngx_http_upstream_rr_peers_unlock(peers);
            return NGX_ERROR;
        }

        if (peers->next
            && ngx_http_json_status_peers(w, peers->next, 1, &pfirst)
               != NGX_OK)
        {
            ngx_http_upstream_rr_peers_unlock(peers);
            return NGX_ERROR;
        }

        ngx_http_upstream_rr_peers_unlock(peers);

        p = ngx_http_json_status_reserve(w, sizeof("]}"));
        if (p == NULL) {
            return NGX_ERROR;
        }

        p = ngx_cpymem(p, "]}", sizeof("]}") - 1);
        ngx_http_json_status_commit(w, p);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_json_status_handler(ngx_http_request_t *r)
{
    u_char                        *p;
    size_t                         need;
    ngx_int_t                      rc;
    ngx_atomic_int_t               ac, ap, hn, rq, rd, wr, wa;
    ngx_http_json_status_writer_t  w;

    if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_HEAD))) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);

    if (rc != NGX_OK) {
        return rc;
    }

    ngx_memzero(&w, sizeof(ngx_http_json_status_writer_t));
    w.r = r;
    w.last = &w.out;

    ac = *ngx_stat_active;
    ap = *ngx_stat_accepted;
    hn = *ngx_stat_handled;
    rq = *ngx_stat_requests;
    rd = *ngx_stat_reading;
    wr = *ngx_stat_writing;
    wa = *ngx_stat_waiting;

    need = sizeof("{\"nginx\":{\"version\":\"" NGINX_VERSION "\",\"pid\":},"
                  "\"connections\":{\"active\":,\"accepted\":,\"handled\":,"
                  "\"reading\":,\"writing\":,\"waiting\":},"
                  "\"requests\":{\"total\":},\"upstreams\":{")
           + NGX_INT_T_LEN + 7 * NGX_ATOMIC_T_LEN;

    p = ngx_http_json_status_reserve(&w, need);
    if (p == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    p = ngx_sprintf(p, "{\"nginx\":{\"version\":\"" NGINX_VERSION "\""
                       ",\"pid\":%P}", ngx_pid);

    p = ngx_sprintf(p, ",\"connections\":{\"active\":%uA,\"accepted\":%uA"
                       ",\"handled\":%uA,\"reading\":%uA,\"writing\":%uA"
                       ",\"waiting\":%uA}", ac, ap, hn, rd, wr, wa);

    p = ngx_sprintf(p, ",\"requests\":{\"total\":%uA}", rq);

    p = ngx_cpymem(p, ",\"upstreams\":{", sizeof(",\"upstreams\":{") - 1);

    ngx_http_json_status_commit(&w, p);

    if (ngx_http_json_status_upstreams(&w) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    p = ngx_http_json_status_reserve(&w, sizeof("}}\n"));
    if (p == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    p = ngx_cpymem(p, "}}\n", sizeof("}}\n") - 1);
    ngx_http_json_status_commit(&w, p);

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = w.len;

    r->headers_out.content_type_len = sizeof("application/json") - 1;
    ngx_str_set(&r->headers_out.content_type, "application/json");
    r->headers_out.content_type_lowcase = NULL;

    w.buf->last_buf = (r == r->main) ? 1 : 0;
    w.buf->last_in_chain = 1;

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, w.out);
}


static char *
ngx_http_json_status(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_json_status_handler;

    return NGX_CONF_OK;
}
