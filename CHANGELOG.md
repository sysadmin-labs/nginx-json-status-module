# Changelog

Newest first.  Dates are release dates.

## v0.1.0 (2026-10-10)

### Added

- The module, written from scratch.  `json_status;` in a location answers
  nginx's connection and request counters and the servers of every
  `upstream` block as one JSON object, with `Content-Type:
  application/json`.  `GET` and `HEAD` are answered, anything else with
  `405`.
- Why it exists rather than being taken over from somewhere: the old
  `ngx_http_json_status_module` carries **no licence**, in no commit of its
  history, and its five contributors would all have to agree to one.  So
  this is not that module -- it shares no line with it -- and it is
  BSD 2-Clause like everything else of ours.
- And why it exists at all: nginx offers `stub_status`, seven numbers as
  plain text, with nothing about the servers behind an upstream.  The
  control API that arrived in nginx 1.31.5 (`nginx -l addr`, `/1/nginx`,
  `/1/control/config`, `/1/control/processes`) is configuration and process
  control, not traffic statistics.  Nothing in the free nginx emits JSON
  status.
- Per-server numbers are reported only for an upstream that carries a
  `zone`, and that is a decision, not a gap.  Without shared memory the
  peer list is private to each worker, so the numbers would describe
  whichever worker answered; and without a zone `us->peer.data` holds
  whatever the configured balancer put there, which is not something to
  read as a round robin peer list on the off chance.  An upstream without a
  zone is reported as `"zone": false` with an empty peer list.
- Only an upstream declared with an `upstream` block appears.  This was
  measured, not assumed: `proxy_pass http://127.0.0.1:18799` registers an
  upstream of its own named `127.0.0.1`, **with** a server entry, so
  counting servers does not tell the two apart.  `NGX_HTTP_UPSTREAM_CREATE`
  does, because only the `upstream` directive sets it.  The first draft
  filtered on the server count and listed the invented upstream; `t/upstreams.t`
  TEST 6 holds the fix down.
- The answer is written into a chain of buffers that grows as it is filled,
  not into one buffer sized by a guess.  Guessing would mean walking the
  peer list twice, once to measure and once to write, and a peer can come
  or go between the two.
- `NGX_STAT_STUB` is set by the module's own `config`.  The counters and the
  code in eleven of nginx's sources that increments them all live behind
  that define, and nginx only sets it for its stub status module, which is
  off by default.  A **loadable** build cannot do that -- there the counters
  are symbols in a binary that was built earlier -- so the `config` refuses
  a loadable build unless nginx itself has `--with-http_stub_status_module`,
  instead of producing a module that fails to load on an undefined symbol.
  Proven: that configure run ends with exit 1 and says why.
- Checked on Linux and FreeBSD, statically and as a loadable object, against
  nginx 1.28.3, 1.30.5 and 1.31.6: 86 tests over `t/basic.t` and
  `t/upstreams.t`, the reload gate with eight reloads, the sanitizers and
  valgrind.
