FROM gcc:14
WORKDIR /app
COPY Makefile demo.sh ./
COPY src/ src/
RUN make
CMD ["./demo.sh"]