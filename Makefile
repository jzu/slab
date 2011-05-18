#
# Sorry, no install yet
#

slab: slab.c fix_fft.o
	gcc -Wall -g -lasound -lpthread -lm -o $@ $< fix_fft.o

fix_fft.o: fix_fft.c
	gcc -Wall -g -c -o $@ $<

fix_fft: fix_fft.c fix_fft.h
	gcc -Wall -g -lm -DMAIN -o $@ $<

clean:
	/bin/rm slab fix_fft.o fix_fft
