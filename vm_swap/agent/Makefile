all: client_qemu server_qemu

client_qemu: client_qemu.cc
	g++ client_qemu.cc -o client_qemu

server_qemu: server_qemu.cc
	g++ server_qemu.cc -o server_qemu -static-libstdc++ -static-libgcc

clean:
	rm -f client_qemu server_qemu

.PHONY: all clean
