# apt install $(CC)
THIS_FILE := $(lastword $(MAKEFILE_LIST))


OBJ=obj
SRC=src
CC=gcc
CFLAGS=-O2
# -Wall

DEBUG=-g

plain: pimera

zero: CFLAGS+=-DPI_ZERO=1 -D__ARM_ACLE=1
zero: pimera

three: CFLAGS+=-DPI_THREE=1
three: CFLAGS+=-mfpu=neon
three: pimera

four: CFLAGS+=-DPI_FOUR=1
four: CFLAGS+=-mfpu=neon
four: pimera


pimera: $(OBJ)/main.o $(OBJ)/http.o $(OBJ)/log.o $(OBJ)/detection.o $(OBJ)/camera.o $(OBJ)/mjpeg.o
	$(CC) $(DEBUG) $(CFLAGS) -o pimera \
		-Wl,--no-as-needed \
		-lmmal_core -lmmal_util -lmmal_vc_client -lvcos -lbcm_host -lm -lpthread \
		$(OBJ)/main.o $(OBJ)/http.o $(OBJ)/log.o $(OBJ)/detection.o $(OBJ)/camera.o $(OBJ)/mjpeg.o

$(OBJ)/main.o: $(SRC)/main.c
	$(CC) $(CFLAGS) -o $@ \
		-c $(SRC)/main.c

$(OBJ)/http.o: $(SRC)/http.c
	$(CC) $(CFLAGS) -o $@ \
		-c $(SRC)/http.c

$(OBJ)/log.o: $(SRC)/log.c
	$(CC) $(CFLAGS) -o $@ \
		-c $(SRC)/log.c

$(OBJ)/detection.o: $(SRC)/detection.c
	$(CC) $(CFLAGS) -o $@ \
		-c $(SRC)/detection.c

$(OBJ)/camera.o: $(SRC)/camera.c
	$(CC) $(CFLAGS) -o $@ \
		-c $(SRC)/camera.c

$(OBJ)/mjpeg.o: $(SRC)/mjpeg.c
	$(CC) $(CFLAGS) -o $@ \
		-c $(SRC)/mjpeg.c
