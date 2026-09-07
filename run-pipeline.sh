#!/bin/sh
# Runs the whole chain in ONE network namespace, which is mandatory: MCAST_IF
# is 127.0.0.1, so exchange, feedhandler and any subscriber only see each
# other's multicast if they share a loopback interface. Separate containers on
# a bridge network do not.
#
# Startup order is a dependency chain, not a preference:
#   feedhandler must have JOINED the group before exchange sends,
#   and any subscriber wants to be up before feedhandler republishes.
set -e
cd /app

# Linux needs an explicit multicast route on loopback; macOS does not. Harmless
# if it is already there, and skipped without NET_ADMIN.
ip route add 224.0.0.0/4 dev lo 2>/dev/null || true

./bin/feedhandler &
FH=$!
sleep 1
./bin/exchange
# exchange returns once the capture is exhausted; feedhandler notices the
# silence after FEED_IDLE_STOP_MS, sends its 'E' terminator, and reports.
wait $FH
