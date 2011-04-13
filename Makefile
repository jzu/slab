#
# Sorry, no install yet
#

slab: slab.c
	gcc -Wall -g -lasound -lpthread -lm -o $@ $<

clean:
	/bin/rm slab
