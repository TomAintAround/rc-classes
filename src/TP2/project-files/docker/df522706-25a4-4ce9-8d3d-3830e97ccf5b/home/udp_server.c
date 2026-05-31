#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUFLEN 512 // Tamanho do buffer
#define PORT 80	   // Porto para recepção das mensagens (changed)

int udp_socket; // add udp socket

void erro(char* s) {
	perror(s);
	exit(1);
}

// add receive_message function
void receive_message(char* buffer, int bufsize) {
	struct sockaddr_in si_outra;
	socklen_t slen = sizeof(si_outra);
	int recv_len;

	// Espera recepção de mensagem (a chamada é bloqueante)
	if ((recv_len = recvfrom(udp_socket, buffer, bufsize, 0,
							 (struct sockaddr*)&si_outra, &slen)) == -1) {
		erro("Erro no recvfrom");
	}
	// Para ignorar o restante conteúdo (anterior do buffer)
	buffer[recv_len] = '\0';

	printf("Recebi uma mensagem do sistema com o endereço %s e o porto %d\n",
		   inet_ntoa(si_outra.sin_addr), ntohs(si_outra.sin_port));
	printf("Conteúdo da mensagem: %s\n", buffer);
}

// add send_message function
void send_message(const char* destIP, int destPort, const char* buffer, int bufsize) {
	struct sockaddr_in si_dest;
	memset(&si_dest, 0, sizeof(si_dest));
	si_dest.sin_family = AF_INET;
	si_dest.sin_port = htons(destPort);
	inet_pton(AF_INET, destIP, &si_dest.sin_addr);

	if (sendto(udp_socket, buffer, bufsize, 0, (struct sockaddr*)&si_dest,
			   sizeof(si_dest)) == -1) {
		erro("Erro no sendto");
	}

	printf("Enviado para %s:%d: %s\n", destIP, destPort, buffer);
}

int main(int argc, char** argv) {
	// remove some variables (used in other functions)
	struct sockaddr_in si_minha;
	char buf[BUFLEN];

	// make sure at least 1 argument is present
	if (argc < 2) {
		printf("Uso:\n");
		printf("Modo servidor: %s server\n", argv[0]);
		printf("Modo cliente: %s client <IP servidor> <porto do servidor>\n", argv[0]);
		exit(1);
	}

	// check if IP is an argument when it's a client
	if (strncmp("client", argv[1], BUFLEN - 1) == 0 && argc < 4) {
		printf("Uso:\n");
		printf("Modo cliente: %s client <IP servidor> <porto do servidor>\n", argv[0]);
	}

	// Cria um socket para recepção de pacotes UDP
	if ((udp_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1) {
		erro("Erro na criação do socket");
	}

	// Preenchimento da socket address structure
	si_minha.sin_family = AF_INET;
	si_minha.sin_port = htons(PORT);
	si_minha.sin_addr.s_addr = htonl(INADDR_ANY);

	// Associa o socket à informação de endereço
	if (bind(udp_socket, (struct sockaddr*)&si_minha, sizeof(si_minha)) == -1) {
		erro("Erro no bind");
	}

	// chose between server and client
	if (strncmp(argv[1], "server", BUFLEN - 1) == 0) {
		printf("Iniciando servidor\n");
		while (1) receive_message(buf, BUFLEN);
	} else if (strncmp(argv[1], "client", BUFLEN - 1) == 0) {
		printf("Escreve a mensagem: ");
		fgets(buf, BUFLEN, stdin);
		send_message(argv[2], atoi(argv[3]), buf, (int)strlen(buf));
	}

	// Fecha socket e termina programa
	close(udp_socket);
	return 0;
}
