# vi:filetype=perl

# Per-server numbers are only reported for an upstream that carries a
# "zone".  Without one, us->peer.data holds whatever the configured
# balancer put there and reading it as a round robin peer list would be a
# guess; with one, the zone module builds that list itself.  These cases
# nail down both halves of that rule.

use lib 'lib';
use Test::Nginx::Socket;

repeat_each(2);

plan tests => repeat_each() * (3 * blocks());

run_tests();

__DATA__

=== TEST 1: an upstream without a zone is named, but has no peers
--- http_config
    upstream plain {
        server 127.0.0.1:18891;
        server 127.0.0.1:18892;
    }
--- config
    location /status {
        json_status;
    }
--- request
GET /status
--- response_body_like: "plain":\{"zone":false,"peers":\[\]\}
--- no_error_log
[error]



=== TEST 2: with a zone every server shows up with its own numbers
--- http_config
    upstream zoned {
        zone zoned 64k;
        server 127.0.0.1:18893;
    }
--- config
    location /status {
        json_status;
    }
--- request
GET /status
--- response_body_like: "zoned":\{"zone":true,"peers":\[\{"server":"127\.0\.0\.1:18893","name":"127\.0\.0\.1:18893","weight":1,"conns":0,"max_conns":0,"fails":0,"max_fails":1,"fail_timeout":10,"down":false,"backup":false\}\]\}
--- no_error_log
[error]



=== TEST 3: max_fails and fail_timeout are the configured ones
--- http_config
    upstream tuned {
        zone tuned 64k;
        server 127.0.0.1:18894 weight=3 max_fails=5 fail_timeout=7s max_conns=9;
    }
--- config
    location /status {
        json_status;
    }
--- request
GET /status
--- response_body_like: "weight":3,"conns":0,"max_conns":9,"fails":0,"max_fails":5,"fail_timeout":7
--- no_error_log
[error]



=== TEST 4: a server marked down says so
--- http_config
    upstream withdown {
        zone withdown 64k;
        server 127.0.0.1:18895;
        server 127.0.0.1:18896 down;
    }
--- config
    location /status {
        json_status;
    }
--- request
GET /status
--- response_body_like: "server":"127\.0\.0\.1:18896".*"down":true
--- no_error_log
[error]



=== TEST 5: a backup server is marked as one
--- http_config
    upstream withbackup {
        zone withbackup 64k;
        server 127.0.0.1:18897;
        server 127.0.0.1:18898 backup;
    }
--- config
    location /status {
        json_status;
    }
--- request
GET /status
--- response_body_like: "server":"127\.0\.0\.1:18898".*"backup":true
--- no_error_log
[error]



=== TEST 6: proxy_pass with a literal host does not invent an upstream
--- config
    location /status {
        json_status;
    }
    location /p {
        proxy_pass http://127.0.0.1:18899;
    }
--- request
GET /status
--- response_body_like: "upstreams":\{\}\}$
--- no_error_log
[error]



=== TEST 7: two upstream blocks are both reported
--- http_config
    upstream one {
        zone one 64k;
        server 127.0.0.1:18881;
    }
    upstream two {
        server 127.0.0.1:18882;
    }
--- config
    location /status {
        json_status;
    }
--- request
GET /status
--- response_body_like: "one":\{"zone":true.*"two":\{"zone":false
--- no_error_log
[error]
