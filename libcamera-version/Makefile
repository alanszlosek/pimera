# apt install clang libcamera-dev libjpeg62-turbo-dev
CXX=clang++
CPP_VERSION=-std=c++17
SRC=src
OBJ=obj

OBJS=$(OBJ)/main.o $(OBJ)/annotate.o $(OBJ)/encoder.o $(OBJ)/image.o $(OBJ)/logging.o

OBJS2=$(OBJ)/encoding.o $(OBJ)/encoder.o $(OBJ)/logging.o


pimera: $(OBJS)
	$(CXX) -g $(CPP_VERSION) -o pimera \
		-I /usr/include/libcamera \
		-L /usr/lib/aarch64-linux-gnu \
		-l camera -l camera-base -l jpeg -lpthread \
		$(OBJS)

release: $(OBJS)
	$(CXX) $(CPP_VERSION) -O2 -o pimera \
		-I /usr/include/libcamera \
		-L /usr/lib/aarch64-linux-gnu \
		-l camera -l camera-base -l jpeg -lpthread \
		$(OBJS)

$(OBJ)/main.o: $(SRC)/main.cpp
	$(CXX) $(CPP_VERSION) \
		-I /usr/include/libcamera \
		-L /usr/lib/aarch64-linux-gnu \
		-c $(SRC)/main.cpp -o $@

$(OBJ)/annotate.o: $(SRC)/annotate.cpp
	$(CXX) $(CPP_VERSION) \
		-I /usr/include/libcamera \
		-L /usr/lib/aarch64-linux-gnu \
		-c $(SRC)/annotate.cpp -o $@

$(OBJ)/encoder.o: $(SRC)/encoder.cpp
	$(CXX) $(CPP_VERSION) \
		-I /usr/include/libcamera \
		-c $(SRC)/encoder.cpp -o $@

$(OBJ)/image.o: $(SRC)/image.cpp
	$(CXX) $(CPP_VERSION) \
		-I /usr/include/libcamera \
		-L /usr/lib/aarch64-linux-gnu \
		-c $(SRC)/image.cpp -o $@

$(OBJ)/logging.o: $(SRC)/logging.cpp
	$(CXX) $(CPP_VERSION) \
		-L /usr/lib/aarch64-linux-gnu \
		-c $(SRC)/logging.cpp -o $@


# encoder testing
$(OBJ)/encoding.o: $(SRC)/encoding.cpp
	$(CXX) $(CPP_VERSION) \
		-c $(SRC)/encoding.cpp -o $@

encoding: $(OBJS2)
	$(CXX) -g $(CPP_VERSION) -o encoding \
		-L /usr/lib/aarch64-linux-gnu \
		-l jpeg -lpthread \
		$(OBJS2)

clean:
	rm $(OBJS)
