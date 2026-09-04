CXX      = c++
CXXFLAGS = -std=c++23 -O2 -Wall -Wextra -pthread

# Discovered, not hardcoded: every src/*.cpp is treated as its own program,
# and every src/*.h as a dependency of all of them.
SRCS = $(wildcard src/*.cpp)
HDRS = $(wildcard src/*.h)
BINS = $(patsubst src/%.cpp,bin/%,$(SRCS))

all: $(BINS)

bin/%: src/%.cpp $(HDRS) | bin
	$(CXX) $(CXXFLAGS) -o $@ $<

bin:
	mkdir -p bin

clean:
	rm -rf bin

.PHONY: all clean
