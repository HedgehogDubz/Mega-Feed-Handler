CXX		= c++
CXXFLAGS= -std=c++23 -O2 -Wall -Wextra -pthread

HDRS	= src/test.h
BINS	= bin/test

all: $(BINS)

bin/%: src/%.cpp $(HDRS) | bin
	$(CXX) $(CXXFLAGS) -o $@ $<

bin:
	mkdir -p bin

clean:
	rm -rf bin

.PHONY: all clean