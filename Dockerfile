FROM ubuntu:22.04

# Install build tools
RUN apt-get update && apt-get install -y g++ gcc sqlite3 libsqlite3-dev

WORKDIR /app

# Copy project files
COPY . .

# Compile C source (sqlite3.c) using GCC (C compiler)
RUN gcc -c backend/sqlite3.c -I./backend -o backend/sqlite3.o

# Compile C++ source (main.cpp) and link with sqlite3.o using G++
RUN g++ -std=c++17 -O2 -I./backend backend/main.cpp backend/sqlite3.o -lpthread -ldl -o server

EXPOSE 8080

CMD ["./server"]