AM_CFLAGS=-Wall -Werror -fno-strict-aliasing -I$(top_srcdir)/include
AUTOMAKE_OPTIONS = subdir-objects

bin_PROGRAMS=zip_sva_perf

zip_sva_perf_SOURCES=testsuit.c test_lib.c

if WD_STATIC_DRV
zip_sva_perf_LDADD=../../.libs/libwd.a ../../.libs/libwd_comp.a \
		    ../../.libs/libhisi_zip.a -ldl -lpthread -lnuma $(libcrypto_LIBS)
else
zip_sva_perf_LDADD=-L../../.libs -l:libwd.so.2 -l:libwd_comp.so.2 \
		   -lpthread -lnuma $(libcrypto_LIBS)
endif
zip_sva_perf_LDFLAGS=-Wl,-rpath,'/usr/local/lib'

# For statistics
zip_sva_perf_LDADD+=-lm

if HAVE_ZLIB
zip_sva_perf_LDADD+=-lz
zip_sva_perf_CPPFLAGS=-DUSE_ZLIB
endif
