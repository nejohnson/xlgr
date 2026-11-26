#!/bin/sh
# Command-line handling tests. These fail before any X connection is made, so
# they need no display. Usage: tests/cli.sh ./xlgr
xlgr=${1:-./xlgr}
fails=0
checks=0

# expect STATUS "text the stderr/stdout must contain" args...
expect() {
    want_status=$1; want_text=$2; shift 2
    out=$("$xlgr" "$@" </dev/null 2>&1)
    status=$?
    checks=$((checks + 1))
    case $out in
        *"$want_text"*) found=1 ;;
        *) found=0 ;;
    esac
    if [ "$status" -ne "$want_status" ] || [ "$found" -ne 1 ]; then
        fails=$((fails + 1))
        echo "FAIL: xlgr $* -> status $status, output: $out"
    fi
}

expect 0 "Usage: xlgr" -help
expect 0 "Usage: xlgr" --help
expect 1 "unknown option: -bogus" -bogus
expect 1 "missing value for -serial" -serial
expect 1 "missing value for -rules" -rules
expect 1 "baud must be 50-4000000" -serial /dev/null -baud fast
expect 1 "baud must be 50-4000000" -serial /dev/null -baud 10
expect 1 "scrollback must be 100-100000000" -scrollback 5 x.log
expect 1 "scrollback must be 100-100000000" -scrollback many x.log
expect 1 "only one file may be given" a.log b.log
expect 1 "give only one of a file, -serial and -telnet" a.log -serial /dev/null
expect 1 "give only one of a file, -serial and -telnet" -serial /dev/null -telnet host:23
expect 1 "-telnet needs host:port" -telnet nohost
expect 1 "-telnet needs host:port" -telnet :23
expect 1 "-telnet needs host:port" -telnet host:

echo "$checks checks, $fails failed"
[ "$fails" -eq 0 ]
