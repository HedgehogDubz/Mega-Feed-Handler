<img width="661" height="627" alt="image" src="https://github.com/user-attachments/assets/6a07724d-7a3a-4629-9282-4d635f986b22" />

## Preqrequisites
1. Mac or Linux
2. GCC 14 or Clang


## Before Running
1. Check that pcapng file is correct format, Run:
    ./bin/checkcap
    Should Return something like:
    Your capture:
    checkcap: pcapngs/20241206_IEXTP1_DPLS1.0.pcap
        UDP payloads readable: 156264237
        payload bytes:         12080660484
        capture spans:         35452.8 s of market time
    checkcap: OK -- enough for PCAP_PACKET_COUNT=1000000
    156 M packets over 9.8 hours — a full session. Reading all 24 GB takes ~90 s.

2. On error: Read the exit code / message. Each failure mode is distinct and verified:

| Symptom | Message | Meaning |
| :--- | :--- | :--- |
| missing | `fopen: No such file or directory` | wrong path or not mounted |
| 0–3 bytes | `too short to be a capture file` | download produced nothing |
| classic pcap | `classic pcap is not supported, only pcapng` | wrong format — convert with <br>`mergecap -F pcapng -w out.pcapng in.pcap` |
| not a capture | `not a pcapng file (magic 0x6c6c6568)` | it's an HTML error page or partial gzip |
| short | `only N packets, but PCAP_PACKET_COUNT is 1000000` | usable, just stops early (exit 2) |

3. Make sure all Addresses are correct for your machine


## No Docker
1. make
2. In seperate terminals run:
    ./bin/visualizer
    ./bin/feedhandler
    ./bin/exchange

(You theorhetically can run in any order (as with real life), however you will lose packets meaning the ending statistics may be wrong)

## Docker
1. Build, Run:
    docker build -t megafeedhandler .
2. Run, Run:
    docker run --rm -v "$PWD/pcapngs:/app/pcapngs" megafeedhandler

## Docker with the Visualizer
Everything must share ONE network namespace. MCAST_IF is 127.0.0.1, so separate
containers each get their own loopback and never see each other's multicast.

Option A -- one container, one terminal:
1. Get a shell, Run:
    docker run -it --rm -v "$PWD/pcapngs:/app/pcapngs" megafeedhandler sh
2. At the container prompt (#), paste all four lines:
    ./bin/feedhandler >/tmp/fh.log 2>&1 &
    sleep 1
    ./bin/exchange >/tmp/ex.log 2>&1 &
    ./bin/visualizer
3. Once it says "stream ended", Run:
    cat /tmp/fh.log

(The redirects matter. Without them feedhandler's warnings interleave with the
ladder's screen clearing and shred the display)

Option B -- two host terminals, split view:
1. Start a container that stays alive, Run:
    docker run -d --rm --name mfh -v "$PWD/pcapngs:/app/pcapngs" megafeedhandler sh -c 'sleep 400'
2. Confirm the mount worked, Run:
    docker exec mfh ./bin/checkcap
3. In terminal 1, Run:
    docker exec -it mfh ./bin/visualizer
4. In terminal 2 (a NEW tab, still on your own machine), Run:
    docker exec -it mfh ./run-pipeline.sh
5. When finished, Run:
    docker rm -f mfh


## What You Should See
The visualizer says which group it joined, then draws a ladder ~10 times a second:
    visualizer: MSFT lives in bucket 1 of 4 -- joining only that group
       ------------ spread 1.70 ------------
      last trade: 100 @ 443.43
    stream ended. delivered=270538 repaired=13441 unrecoverable=0 duplicates-ignored=0

unrecoverable=0 next to repaired=13441 is the health signal: every datagram the
5% loss injection threw away was recovered over TCP. feedhandler should report
Capture gaps: 0 and Crossed-book updates: 0.

(delivered is much smaller than Published because the visualizer joins only the
one bucket MSFT hashes to. That is the bucketing working, not loss)


## Why The Order Matters
1. visualizer before feedhandler: EXPECT_FROM = 1 means it demands history from
   sequence 1. Start it late and it repairs backwards; start it very late and
   anything past the 262144-slot ring is gone for good.
2. feedhandler before exchange: multicast does not buffer for late joiners.
   Anything sent before IP_ADD_MEMBERSHIP completes is never delivered, and you
   get Packets received: 0 with no error at all.


## Watching A Different Symbol
The symbol is compile-time, not an argument:
1. Edit src/subscriber_env.h and change WATCH_SYMBOL
2. Run:
    make
    (re-run docker build too, if you are using Docker)


## If The Ladder Stays Empty
┌─────────────────────┬──────────────────────────────────────────────┐
│       Symptom       │                    Cause                     │
├─────────────────────┼──────────────────────────────────────────────┤
│ feed: 0 delivered   │ feedhandler never published -- check its log │
│ forever             │ for Packets received: 0                      │
├─────────────────────┼──────────────────────────────────────────────┤
│ Packets received: 0 │ separate containers each get a private lo,   │
│                     │ so they never see each other. Everything     │
│                     │ must share ONE container                     │
├─────────────────────┼──────────────────────────────────────────────┤
│ fopen: No such      │ the mount is wrong -- run                    │
│ file or directory   │ docker exec mfh ./bin/checkcap               │
├─────────────────────┼──────────────────────────────────────────────┤
│ Capture gaps is     │ the replay outran the handler -- lower       │
│ not 0               │ REPLAY_PPS in src/env.h                      │
├─────────────────────┼──────────────────────────────────────────────┤
│ spread is negative  │ always upstream loss, never a book bug --    │
│                     │ check Capture gaps first                     │
└─────────────────────┴──────────────────────────────────────────────┘
