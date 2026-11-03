FROM ubuntu:22.04

RUN apt-get update && apt-get install -y g++ sqlite3 libsqlite3-dev

WORKDIR /app
COPY . .

RUN g++ -std=c++17 -O2 -I./backend backend/main.cpp backend/sqlite3.c -lpthread -ldl -o server

EXPOSE 8080
CMD ["./server"]