# vi:filetype=

use lib 'lib';
use Test::Nginx::Socket; # 'no_plan';

repeat_each(1);

plan tests => repeat_each() * 2 * blocks();

run_tests();

__DATA__

=== TEST 1: simple calc
--- config
    location /let {
        let $ a 1;
    }
--- request
GET /let
--- error_log: directive needs variable name
--- must_die
