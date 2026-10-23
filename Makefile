# Top-level Makefile for IT305 Socket Programming Project
PARTS := Part1 Part2_Case1 Part2_Case2 Part2_Case2_Enhanced

all:
	@for p in $(PARTS); do $(MAKE) -C $$p all || exit 1; done

clean:
	@for p in $(PARTS); do $(MAKE) -C $$p clean || exit 1; done
	rm -rf out test_out .staging_*

.PHONY: all clean
