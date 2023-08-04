# apt install $(CC)

OBJ=obj
SRC=src
CC=gcc
NEON=-mfpu=neon
#DEBUG=-g

pimera: $(OBJ)/main.o $(OBJ)/http.o $(OBJ)/log.o $(OBJ)/detection.o $(OBJ)/camera.o $(OBJ)/mjpeg.o
	$(CC) $(DEBUG) -o pimera \
		-Wl,--no-as-needed \
		-lmmal_core -lmmal_util -lmmal_vc_client -lvcos -lbcm_host -lm -lpthread \
		$(OBJ)/main.o $(OBJ)/http.o $(OBJ)/log.o $(OBJ)/detection.o $(OBJ)/camera.o $(OBJ)/mjpeg.o

$(OBJ)/main.o: $(SRC)/main.c
	$(CC) $(NEON) $(DEBUG) -o $@ \
		-c $(SRC)/main.c

$(OBJ)/http.o: $(SRC)/http.c
	$(CC) $(DEBUG) -o $@ \
		-c $(SRC)/http.c

$(OBJ)/log.o: $(SRC)/log.c
	$(CC) $(DEBUG) -o $@ \
		-c $(SRC)/log.c

$(OBJ)/detection.o: $(SRC)/detection.c
	$(CC) $(NEON) $(DEBUG) -o $@ \
		-c $(SRC)/detection.c

$(OBJ)/camera.o: $(SRC)/camera.c
	$(CC) $(DEBUG) -o $@ \
		-c $(SRC)/camera.c

$(OBJ)/mjpeg.o: $(SRC)/mjpeg.c
	$(CC) $(DEBUG) -o $@ \
		-c $(SRC)/mjpeg.c

debug: src/main.c
	$(CC) -g -o pimera \
		-lmmal_core -lmmal_util -lmmal_vc_client -lvcos -lbcm_host -lm -lpthread \
		src/main.c
