FROM gcc:14

WORKDIR /app

# Build inputs only. The Makefile discovers src/*.cpp and src/*.h with wildcard,
# so adding or removing programs here needs no change to this file.
COPY Makefile ./
COPY src/ src/

RUN make -j"$(nproc)"

# Captures live in pcapngs/, which .dockerignore excludes (they are multi-GB).
# Mount one at run time so PCAP_FILE (src/env.h, relative to /app) resolves:
#   docker run --rm -v "$PWD/pcapngs:/app/pcapngs" megafeedhandler
CMD ["sh", "-c", "[ -x bin/exchange ] && exec ./bin/exchange || { echo 'built binaries:'; ls -1 bin; }"]
