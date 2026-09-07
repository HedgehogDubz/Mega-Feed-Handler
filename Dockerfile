FROM gcc:14

WORKDIR /app

# Build inputs only. The Makefile discovers src/*.cpp and src/*.h with wildcard,
# so adding or removing programs here needs no change to this file.
COPY Makefile ./
COPY src/ src/

# Building here is not just packaging: it is the only place this code sees a
# second compiler and a second OS. GCC 14 catches what Apple clang accepts,
# and Linux exercises the #else half of every platform conditional -- which is
# where a SIGPIPE that kills the whole publisher was found hiding.
RUN make -j"$(nproc)" && make check

COPY run-pipeline.sh ./

CMD ["./run-pipeline.sh"]
