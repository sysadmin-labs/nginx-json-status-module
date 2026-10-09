About
=====

[![Build & Test][build-test-badge]][build-test-link]
[![FreeBSD][freebsd-badge]][freebsd-link]
[![A/UBSan][sanitizers-badge]][sanitizers-link]
[![Valgrind][valgrind-badge]][valgrind-link]
[![Reload][reload-badge]][reload-link]
[![CodeQL][codeql-badge]][codeql-link]
[![Lint][lint-badge]][lint-link]

nginx-json-status-module - nginx's connection and request counters, and the
servers of every upstream, as one JSON object.

[build-test-badge]: https://github.com/sysadmin-labs/nginx-json-status-module/actions/workflows/build-test.yml/badge.svg
[build-test-link]: https://github.com/sysadmin-labs/nginx-json-status-module/actions/workflows/build-test.yml
[freebsd-badge]: https://github.com/sysadmin-labs/nginx-json-status-module/actions/workflows/freebsd.yml/badge.svg
[freebsd-link]: https://github.com/sysadmin-labs/nginx-json-status-module/actions/workflows/freebsd.yml
[sanitizers-badge]: https://github.com/sysadmin-labs/nginx-json-status-module/actions/workflows/sanitizers.yml/badge.svg
[sanitizers-link]: https://github.com/sysadmin-labs/nginx-json-status-module/actions/workflows/sanitizers.yml
[valgrind-badge]: https://github.com/sysadmin-labs/nginx-json-status-module/actions/workflows/valgrind.yml/badge.svg
[valgrind-link]: https://github.com/sysadmin-labs/nginx-json-status-module/actions/workflows/valgrind.yml
[reload-badge]: https://github.com/sysadmin-labs/nginx-json-status-module/actions/workflows/reload.yml/badge.svg
[reload-link]: https://github.com/sysadmin-labs/nginx-json-status-module/actions/workflows/reload.yml
[codeql-badge]: https://github.com/sysadmin-labs/nginx-json-status-module/actions/workflows/codeql.yml/badge.svg
[codeql-link]: https://github.com/sysadmin-labs/nginx-json-status-module/actions/workflows/codeql.yml
[lint-badge]: https://github.com/sysadmin-labs/nginx-json-status-module/actions/workflows/lint.yml/badge.svg
[lint-link]: https://github.com/sysadmin-labs/nginx-json-status-module/actions/workflows/lint.yml

Status
======

In use.  The module builds against nginx 1.28.3, 1.30.5 and 1.31.6, both
statically and as a loadable object, and on each of them the endpoint
answers a JSON object that a parser accepts.  That is checked on Linux and
on FreeBSD.

What nginx itself offers is `stub_status`: seven numbers as plain text, and
nothing about the servers behind an upstream.  The control API that arrived
in nginx 1.31.5 is configuration and process control, not traffic
statistics.  This module fills the gap in between, and nothing else.

Configuration directives
========================

json_status
-----------

* **syntax**: `json_status;`
* **context**: location

Answers the request with the status object.  Takes no arguments.  `GET` and
`HEAD` are answered, anything else with `405`.

Output
======

```json
{
  "nginx": { "version": "1.30.5", "pid": 42172 },
  "connections": {
    "active": 1, "accepted": 1, "handled": 1,
    "reading": 0, "writing": 1, "waiting": 0
  },
  "requests": { "total": 1 },
  "upstreams": {
    "plain": { "zone": false, "peers": [] },
    "zoned": {
      "zone": true,
      "peers": [
        {
          "server": "127.0.0.1:18793", "name": "127.0.0.1:18793",
          "weight": 1, "conns": 0, "max_conns": 0,
          "fails": 0, "max_fails": 2, "fail_timeout": 7,
          "down": false, "backup": false
        }
      ]
    }
  }
}
```

The counters under `connections` and `requests` are nginx's own, the ones
`stub_status` prints.  They are per worker process unless nginx was built
with shared memory for them, which it is here.

`conns` is the number of requests a server is handling right now, `fails`
the number of failures counted inside the current `fail_timeout`.

Only an upstream declared with an `upstream` block appears.  A `proxy_pass`
with a literal host registers an upstream of its own, named after the host,
and that one is left out.

**Per-server numbers need a `zone`.** Without one, an upstream is reported
as `"zone": false` with an empty peer list, for two reasons.  The peer list
of an upstream without shared memory is private to each worker, so the
numbers would describe whichever worker happened to answer.  And without a
zone the list is whatever the configured balancer put there, which is not
something to read as a round robin peer list on the off chance.

Sample configuration
====================

```nginx
http {
    upstream backend {
        zone backend 64k;
        server 127.0.0.1:8081 max_fails=2 fail_timeout=7s;
        server 127.0.0.1:8082 backup;
    }

    server {
        listen 127.0.0.1:8080;

        location /status {
            json_status;

            allow 127.0.0.1;
            deny all;
        }

        location / {
            proxy_pass http://backend;
        }
    }
}
```

Requirements
============

The counters live behind nginx's `NGX_STAT_STUB`, and so does the code in
nginx that increments them.  The module's `config` sets that define, so a
**static** build with `--add-module` needs nothing else.

A **loadable** build is different: the counters are symbols in the nginx
binary, and they are only there if that binary was built with
`--with-http_stub_status_module`.  The `config` refuses a loadable build
without it rather than producing a module that fails to load on an
undefined symbol.

Test Suite
==========

The suite is written against
[Test::Nginx](https://metacpan.org/pod/Test::Nginx):

```
ci/build.sh 1.31.6 /tmp/nginx-test
TEST_NGINX_BINARY=/tmp/nginx-test/sbin/nginx prove -r t/
```

`ci/reload.sh` reloads nginx eight times and after every one of them asks
the module for the zoned upstream again, because the peer list it reads
lives in shared memory that an earlier cycle allocated.

License
=======

BSD 2-Clause, see [LICENSE](LICENSE).
