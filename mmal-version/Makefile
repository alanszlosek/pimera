# apt install clang

OBJ=obj
SRC=src
#DEBUG=-g

pimera: $(OBJ)/main.o $(OBJ)/http.o $(OBJ)/log.o $(OBJ)/detection.o $(OBJ)/camera.o $(OBJ)/mjpeg.o
	clang $(DEBUG) -o pimera \
		-lmmal_core -lmmal_util -lmmal_vc_client -lvcos -lbcm_host -lm -lpthread \
		$(OBJ)/main.o $(OBJ)/http.o $(OBJ)/log.o $(OBJ)/detection.o $(OBJ)/camera.o $(OBJ)/mjpeg.o

$(OBJ)/main.o: $(SRC)/main.c
	clang $(DEBUG) -o $@ \
		-c $(SRC)/main.c

$(OBJ)/http.o: $(SRC)/http.c
	clang $(DEBUG) -o $@ \
		-c $(SRC)/http.c

$(OBJ)/log.o: $(SRC)/log.c
	clang $(DEBUG) -o $@ \
		-c $(SRC)/log.c

$(OBJ)/detection.o: $(SRC)/detection.c
	clang $(DEBUG) -o $@ \
		-c $(SRC)/detection.c

$(OBJ)/camera.o: $(SRC)/camera.c
	clang $(DEBUG) -o $@ \
		-c $(SRC)/camera.c

$(OBJ)/mjpeg.o: $(SRC)/mjpeg.c
	clang $(DEBUG) -o $@ \
		-c $(SRC)/mjpeg.c

debug: src/main.c
	clang -g -o pimera \
		-lmmal_core -lmmal_util -lmmal_vc_client -lvcos -lbcm_host -lm -lpthread \
		src/main.c
