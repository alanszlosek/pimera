# apt install clang

OBJ=obj
SRC=src

pimera: $(OBJ)/main.o $(OBJ)/http.o $(OBJ)/log.o $(OBJ)/detection.o
	clang -O2 -o pimera \
		-lmmal_core -lmmal_util -lmmal_vc_client -lvcos -lbcm_host -lm -lpthread \
		$(OBJ)/main.o $(OBJ)/http.o $(OBJ)/log.o $(OBJ)/detection.o

$(OBJ)/main.o: $(SRC)/main.c
	clang -o $@ \
		-c $(SRC)/main.c

$(OBJ)/http.o: $(SRC)/http.c
	clang -o $@ \
		-c $(SRC)/http.c

$(OBJ)/log.o: $(SRC)/log.c
	clang -o $@ \
		-c $(SRC)/log.c

$(OBJ)/detection.o: $(SRC)/detection.c
	clang -o $@ \
		-c $(SRC)/detection.c

debug: src/main.c
	clang -g -o pimera \
		-lmmal_core -lmmal_util -lmmal_vc_client -lvcos -lbcm_host -lm -lpthread \
		src/main.c
