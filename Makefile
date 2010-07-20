#
# Sorry, no install yet
#

slab: slab.c
	gcc -Wall -g -lasound -lpthread -o $@ $<
