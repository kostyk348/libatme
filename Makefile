CC = gcc
CFLAGS = -O2 -Wall -Wextra -march=native -I.
LDFLAGS = -lm
FT_CFLAGS = $(shell pkg-config --cflags freetype2)
FT_LIBS   = $(shell pkg-config --libs freetype2)

OBJS = atm_outline.o atm_raster.o atm_cache.o atm_gamma.o atm_engine.o atm_font.o atm_bench.o

.PHONY: all clean

all: libatme.a atm_bench atm_ft_bench

atm_ft_bench: atm_ft_bench.c libatme.a
	$(CC) $(CFLAGS) $(FT_CFLAGS) -o $@ atm_ft_bench.c libatme.a $(FT_LIBS) $(LDFLAGS)

libatme.a: $(OBJS)
	ar rcs $@ $^

atm_bench: libatme.a atm_bench.o
	$(CC) $(CFLAGS) -o $@ atm_bench.o libatme.a $(LDFLAGS)

atm_outline.o: atm_outline.c atm_outline.h atm_types.h
	$(CC) $(CFLAGS) -c -o $@ $<

atm_raster.o: atm_raster.c atm_raster.h atm_types.h
	$(CC) $(CFLAGS) -c -o $@ $<

atm_cache.o: atm_cache.c atm_cache.h atm_types.h atm_raster.h
	$(CC) $(CFLAGS) -c -o $@ $<

atm_gamma.o: atm_gamma.c atm_gamma.h atm_types.h
	$(CC) $(CFLAGS) -c -o $@ $< -lm

atm_engine.o: atm_engine.c atm_engine.h
	$(CC) $(CFLAGS) -c -o $@ $<

atm_font.o: atm_font.c atm_font.h atm_types.h
	$(CC) $(CFLAGS) -c -o $@ $<

atm_bench.o: atm_bench.c atm_bench.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o libatme.a atm_bench atm_ft_bench
