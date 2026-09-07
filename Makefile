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

# A header nothing #includes yet is never compiled, so `all` can stay green
# while it is broken. This checks each one standalone.
check: $(HDRS)
	@for h in $(HDRS); do \
	  printf '  %-24s' "$$h"; \
	  echo "#include \"$$h\"" > .check.cpp; \
	  $(CXX) $(CXXFLAGS) -fsyntax-only -I. .check.cpp || { rm -f .check.cpp; exit 1; }; \
	  echo ok; \
	done; rm -f .check.cpp

clean:
	rm -rf bin

.PHONY: all check clean
