# vi:filetype=perl

use lib 'lib';
use Test::Nginx::Socket;

repeat_each(2);

# Three checks per block, plus the extra header check in TEST 1.
plan tests => repeat_each() * (3 * blocks() + 1);

run_tests();

__DATA__

=== TEST 1: the answer is JSON, and says so
--- config
    location /status {
        json_status;
    }
--- request
GET /status
--- response_headers
Content-Type: application/json
--- response_body_like: ^\{"nginx":\{.*\}\}$
--- no_error_log
[error]



=== TEST 2: the version and the pid of the worker that answered
--- config
    location /status {
        json_status;
    }
--- request
GET /status
--- response_body_like: "nginx":\{"version":"\d+\.\d+\.\d+","pid":\d+\}
--- no_error_log
[error]



=== TEST 3: every connection counter is there and is a number
--- config
    location /status {
        json_status;
    }
--- request
GET /status
--- response_body_like: "connections":\{"active":\d+,"accepted":\d+,"handled":\d+,"reading":\d+,"writing":\d+,"waiting":\d+\}
--- no_error_log
[error]



=== TEST 4: the request counter is there
--- config
    location /status {
        json_status;
    }
--- request
GET /status
--- response_body_like: "requests":\{"total":\d+\}
--- no_error_log
[error]



=== TEST 5: without an upstream block the object is empty, not missing
--- config
    location /status {
        json_status;
    }
--- request
GET /status
--- response_body_like: "upstreams":\{\}\}$
--- no_error_log
[error]



=== TEST 6: a HEAD request is answered without a body
--- config
    location /status {
        json_status;
    }
--- request
HEAD /status
--- response_headers
Content-Type: application/json
--- response_body
--- no_error_log
[error]



=== TEST 7: anything but GET and HEAD is refused
--- config
    location /status {
        json_status;
    }
--- request
POST /status
body=1
--- error_code: 405
--- no_error_log
[error]
