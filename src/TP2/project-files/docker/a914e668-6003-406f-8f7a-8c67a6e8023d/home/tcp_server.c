/*******************************************************************************
 * SERVIDOR no porto 80, à escuta de novos clientes. Quando surgem novos
 * clientes os dados por eles enviados são lidos e descarregados no ecrã.
 *******************************************************************************/
#include <stdbool.h> // add new library
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <netdb.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <arpa/inet.h> // add new library

#define SERVER_PORT 80 // changed port
#define BUF_SIZE 1024

struct RegisterMessage {
	char psk[64];
};

void process_client(int client_fd, int clientNum, struct sockaddr_in client_addr); // receive more arguments
bool foundPsk(struct RegisterMessage psk, const char* clientIP); // add function to look for psk
void erro(char* msg);

int main() {
	int fd, client;
	struct sockaddr_in addr, client_addr;
	int client_addr_size;

	bzero((void*)&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(SERVER_PORT);

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) erro("na funcao socket");
	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
		erro("na funcao bind");
	if (listen(fd, 5) < 0) erro("na funcao listen");
	client_addr_size = sizeof(client_addr);
	int numConnectedClients = 0; // store number of connected clients
	while (1) {
		// clean finished child processes, avoiding zombies
		// must use WNOHANG or would block whenever a child process was working
		while (waitpid(-1, NULL, WNOHANG) > 0);
		// wait for new connection
		client = accept(fd, (struct sockaddr*)&client_addr, (socklen_t*)&client_addr_size);
		if (client > 0) {
			numConnectedClients++; // increase connect clients count
			if (fork() == 0) {
				close(fd);
				process_client(client, numConnectedClients, client_addr); // add 3 more arguments
				exit(0);
			}
			close(client);
		}
	}

	return 0;
}

void process_client(int client_fd, int clientNum, struct sockaddr_in client_addr) { // receive more arguments
	size_t nread = 0; // change type
	char buffer[BUF_SIZE];

	// get client IP and port
	char clientIP[16];
	inet_ntop(AF_INET, &client_addr.sin_addr, clientIP, INET_ADDRSTRLEN);
	int clientPort = client_addr.sin_port;

	// remove read command (not necessary here)
	printf("Client %d connecting from (IP:port) %s:%d\n", clientNum, clientIP,
		   clientPort); // add more info for print
	fflush(stdout);

	// write initial response
	strncpy(buffer, "Bem vindo ao servidor, deverá autenticar-se com a sua chave pré-definida",
			BUF_SIZE - 1);
	write(client_fd, buffer, strlen(buffer));

	// read psk
	struct RegisterMessage msg;
	nread = read(client_fd, buffer, 63);
	if (nread <= 0) {
		close(client_fd);
		erro("Failed reading message");
	}
	buffer[nread] = '\0';
	strncpy(msg.psk, buffer, 63);

	// write psk response
	if (foundPsk(msg, clientIP))
		snprintf(buffer, BUF_SIZE - 1, "Autenticação aceite, chave fornecida corresponde à definida para o endereço %s",
				 clientIP);
	else
		snprintf(buffer, BUF_SIZE - 1,
				 "Autenticação não aceite, chave inválida para o endereço %s", clientIP);
	write(client_fd, buffer, strlen(buffer));

	// await "SAIR" message
	bool canExit = false;
	while (!canExit) {
		nread = read(client_fd, buffer, BUF_SIZE - 1);
		if (nread <= 0) continue;
		buffer[nread] = '\0';

		if (strncmp("SAIR", buffer, BUF_SIZE - 1) == 0) {
			strncpy(buffer, "Até logo!", BUF_SIZE - 1);
			write(client_fd, buffer, strlen(buffer));
			canExit = true;
		} else {
			write(client_fd, "\0", 1);
		}
	}

	// show when client if disconnecting
	printf("Client %d disconnecting from (IP:port) %s:%d\n", clientNum, clientIP, clientPort);
	fflush(stdout);
	close(client_fd);
}

// add function to look for psk
bool foundPsk(struct RegisterMessage msg, const char* clientIP) {
	FILE* file = fopen("psk.txt", "r");
	if (file == NULL) return false;

	char line[BUF_SIZE];
	while (fgets(line, BUF_SIZE, file)) {
		line[strlen(line) - 1] = '\0';

		char* token = strtok(line, " ");
		if (strncmp(token, clientIP, BUF_SIZE - 1) != 0) continue;

		token = strtok(NULL, " ");
		if (strncmp(token, msg.psk, BUF_SIZE - 1) == 0) {
			fclose(file);
			return true;
		}
	}

	fclose(file);
	return false;
}

void erro(char* msg) {
	printf("Erro: %s\n", msg);
	exit(-1);
}
