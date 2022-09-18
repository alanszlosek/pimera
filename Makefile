# apt install clang libcamera-dev libjpeg62-turbo-dev
SRC=src
OBJ=obj

CPPS=$(wildcard $(SRC)/*.cpp)
OBJS=$(patsubst $(SRC)/%.cpp, $(OBJ)/%.o, $(CPPS))


pimera: $(OBJS)
	clang++ -g -std=c++17 -o pimera \
		-I /usr/include/libcamera \
		-L /usr/lib/aarch64-linux-gnu \
		-l camera -l camera-base -l jpeg -lpthread \
		$(OBJS)

# Trigger object rebuild if either cpp or hpp change
# can't use $< as -c flag value since it contains hpp
$(OBJ)/%.o: $(SRC)/%.cpp 
	clang++ -g -std=c++17 \
		-I /usr/include/libcamera \
		-L /usr/lib/aarch64-linux-gnu \
		-l camera -l camera-base \
		-c $(patsubst $(OBJ)/%.o, $(SRC)/%.cpp, $@) -o $@

scaffold: scaffold.cpp
	clang++ -g -std=c++17 -o scaffold \
		-I /usr/include/libcamera \
		-L /usr/lib/aarch64-linux-gnu \
		-l camera -l camera-base \
		scaffold.cpp


clean:
	rm $(OBJS)