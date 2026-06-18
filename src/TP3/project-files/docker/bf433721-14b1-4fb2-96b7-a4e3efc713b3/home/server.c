#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "powerudp.h"

#define TCP_PORT 80
#define MULTICAST_IP "239.0.0.1"
#define MULTICAST_PORT 9000

int multicast_socket = -1;
struct sockaddr_in mcast_addr;

char expected_psk[64] = "";

// Función para cargar la contraseña desde el archivo psk_key.txt
void load_psk() {
	FILE* file = fopen("psk_key.txt", "r");
	if (file == NULL) {
		perror(
		"[-] Erro crítico: Não foi possível abrir o ficheiro 'psk_key.txt'");
		printf("[!] Assegura-te de criar o arquivo na mesma pasta que o "
			   "executável.\n");
		exit(1);
	}

	if (fgets(expected_psk, sizeof(expected_psk), file) == NULL) {
		fprintf(stderr, "[-] Erro crítico: O ficheiro 'psk_key.txt' está vazio "
						"ou não se pode ler.\n");
		fclose(file);
		exit(1);
	}
	fclose(file);

	expected_psk[strcspn(expected_psk, "\r\n")] = '\0';

	if (strlen(expected_psk) == 0) {
		fprintf(stderr,
				"[-] Erro crítico: A chave lida é inválida ou está vazía.\n");
		exit(1);
	}

	printf("[+] Chave PSK carregada corretamente desde o ficheiro.\n");
}

void broadcast_config(struct ConfigMessage* new_config) {
	if (sendto(multicast_socket, new_config, sizeof(struct ConfigMessage), 0,
			   (struct sockaddr*)&mcast_addr, sizeof(mcast_addr)) < 0) {
		perror("[-] Erro: Falha ao enviar configuração por Multicast");
	} else {
		printf("[+] Configuração Multicast enviada à rede.\n");
	}
}

void handle_client(int client_socket) {
	struct RegisterMessage reg_msg;
	struct ConfigMessage requested_config;

	if (recv(client_socket, &reg_msg, sizeof(reg_msg), 0) <= 0) {
		perror("[-] Erro ao receber registro do cliente");
		close(client_socket);
		exit(1);
	}

	if (strcmp(reg_msg.psk, expected_psk) != 0) {
		printf("[-] Cliente rejeitado: PSK incorreta.\n");
		close(client_socket);
		exit(1);
	}
	printf("[+] Cliente autenticado com êxito.\n");

	while (recv(client_socket, &requested_config, sizeof(requested_config), 0) > 0) {
		printf("[!] Cliente pede mudança de configuração.\n");
		printf("      Retransmissão: %d | Backoff: %d | Sequência: %d | "
			   "Timeout: %dms | Tentativas: %d\n",
			   requested_config.enable_retransmission,
			   requested_config.enable_backoff, requested_config.enable_sequence,
			   requested_config.base_timeout_ms, requested_config.max_retries);

		broadcast_config(&requested_config);
	}

	printf("[-] Cliente desconectado.\n");
	close(client_socket);
	exit(0);
}

int main() {
	int tcp_server_socket, client_socket;
	struct sockaddr_in server_addr, client_addr;
	socklen_t client_len = sizeof(client_addr);

	load_psk();

	multicast_socket = socket(AF_INET, SOCK_DGRAM, 0);
	if (multicast_socket < 0) {
		perror("[-] Erro: Não foi possível criar o socket Multicast");
		return 1;
	}

	memset(&mcast_addr, 0, sizeof(mcast_addr));
	mcast_addr.sin_family = AF_INET;
	mcast_addr.sin_addr.s_addr = inet_addr(MULTICAST_IP);
	mcast_addr.sin_port = htons(MULTICAST_PORT);

	// Desativar loopback para que o servidor não receba as suas próprias mensagens
	char loopback = 0;
	setsockopt(multicast_socket, IPPROTO_IP, IP_MULTICAST_LOOP, &loopback, sizeof(loopback));

	// Aumentar o TTL para que o Multicast atravesse os routers
	unsigned char mc_ttl = 10;
	if (setsockopt(multicast_socket, IPPROTO_IP, IP_MULTICAST_TTL, &mc_ttl,
				   sizeof(mc_ttl)) < 0) {
		perror("[-] Erro: Configuração de TTL do Multicast");
		close(multicast_socket);
		return 1;
	}
	struct in_addr localInterface;
	localInterface.s_addr = inet_addr(INADDR_ANY);
	if (setsockopt(multicast_socket, IPPROTO_IP, IP_MULTICAST_IF,
				   (char*)&localInterface, sizeof(localInterface)) < 0) {
		perror("[-] Erro: Forçar interface de Multicast");
		close(multicast_socket);
		return 1;
	}

	tcp_server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (tcp_server_socket < 0) {
		perror("[-] Erro: Não foi possível criar o socket TCP");
		close(multicast_socket);
		return 1;
	}

	bool reuse_addr = true;
	setsockopt(tcp_server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr));

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = inet_addr(INADDR_ANY);
	server_addr.sin_port = htons(TCP_PORT);

	if (bind(tcp_server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
		perror("[-] Erro: Bind TCP. Possivelmente o porto usado está ocupado");
		close(multicast_socket);
		close(tcp_server_socket);
		return 1;
	}

	if (listen(tcp_server_socket, 5) < 0) {
		perror("[-] Erro: Erro em listen()");
		close(multicast_socket);
		close(tcp_server_socket);
		return 1;
	}

	printf("[+] Servidor iniciado.\n");
	printf("[+] Ouvido registros TCP no porto %d...\n", TCP_PORT);

	while (1) {
		client_socket =
		accept(tcp_server_socket, (struct sockaddr*)&client_addr, &client_len);
		if (client_socket < 0) {
			perror("[-] Erro ao aceitar conexão");
			continue;
		}

		printf("[+] Nova conexão TCP desde %s:%d\n",
			   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

		if (fork() == 0) {
			close(tcp_server_socket);
			handle_client(client_socket);
		} else {
			close(client_socket);
		}
	}

	close(tcp_server_socket);
	close(multicast_socket);
	return 0;
}
