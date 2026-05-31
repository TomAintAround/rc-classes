/**********************************************************************
 * CLIENTE liga ao servidor (definido em argv[1]) no porto especificado
 * (em argv[2]).
 * USO: >cliente <enderecoServidor>  <porto>
 **********************************************************************/
#include <stdbool.h> // add library
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>

#define BUF_SIZE 1024 // add new constant

void erro(char* msg);

int main(int argc, char* argv[]) {
	char endServer[100];
	int fd;
	struct sockaddr_in addr;
	struct hostent* hostPtr;

	// updated client arguments
	if (argc != 3) {
		printf("cliente <host> <port>\n");
		exit(-1);
	}

	strcpy(endServer, argv[1]);
	if ((hostPtr = gethostbyname(endServer)) == 0)
		erro("Não consegui obter endereço");

	bzero((void*)&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ((struct in_addr*)(hostPtr->h_addr))->s_addr;
	addr.sin_port = htons((short)atoi(argv[2]));

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) erro("socket");
	if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) erro("Connect");
	// remove write command (not necessary here)

	// read initial response
	char buffer[1024];
	size_t nread = read(fd, buffer, BUF_SIZE - 1);
	if (nread <= 0) {
		close(fd);
		erro("Failed reading message");
	}
	buffer[nread] = '\0';
	printf("%s\n", buffer);
	fflush(stdout);

	// write psk
	fgets(buffer, 64, stdin);
	buffer[strlen(buffer) - 1] = '\0';
	write(fd, buffer, strlen(buffer));

	// read psk response
	nread = read(fd, buffer, BUF_SIZE - 1);
	if (nread <= 0) {
		close(fd);
		erro("Failed reading message");
	}
	buffer[nread] = '\0';
	printf("%s\n", buffer);
	fflush(stdout);

	bool canExit = false;
	while (!canExit) {
		// write commands ("SAIR")
		fgets(buffer, BUF_SIZE - 1, stdin);
		buffer[strlen(buffer) - 1] = '\0';
		write(fd, buffer, strlen(buffer));

		// read response
		nread = read(fd, buffer, BUF_SIZE - 1);
		if (nread <= 0) continue;
		if (buffer[0] == '\0')
			continue; // in case server sent "\0" (invalid command)
		buffer[nread] = '\0';
		printf("%s\n", buffer);
		fflush(stdout);
		canExit = true;
	}

	close(fd);
	exit(0);
}

void erro(char* msg) {
	printf("Erro: %s\n", msg);
	exit(-1);
}
