#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <signal.h>
#include <pthread.h>
#include "powerudp.h"

struct ClientArgs {
	int socket;
	struct sockaddr_in addr;
};

int multicast_socket = -1;
struct sockaddr_in mcast_addr;

char expected_psk[64] = "";

volatile sig_atomic_t running = 1;

pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;
struct ConfigMessage current_config = { .enable_retransmission = 0,
										.enable_backoff = 0,
										.enable_sequence = 0,
										.base_timeout_ms = 1000,
										.max_retries = 3 };

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

void handle_sigint(int sig) {
	(void)sig;
	running = 0;
}

void* handle_client(void* arg) {
	struct ClientArgs* args = (struct ClientArgs*)arg;
	int client_socket = args->socket;
	struct sockaddr_in client_addr = args->addr;
	free(arg);

	struct RegisterMessage reg_msg;

	if (recv(client_socket, &reg_msg, sizeof(reg_msg), 0) <= 0) {
		perror("[-] Erro: Receber registro do cliente");
		close(client_socket);
		return NULL;
	}

	bool success_login = true;
	if (strcmp(reg_msg.psk, expected_psk) != 0) success_login = false;

	reg_msg.psk[0] = (char)success_login;
	if (send(client_socket, &reg_msg, 1, 0) < 0) {
		perror("Erro: Não foi possível enviar a mensagem de registro");
		close(client_socket);
		return NULL;
	}

	if (success_login)
		printf("[+] Cliente %s:%d autenticado com êxito.\n",
			   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
	else {
		printf("[-] Cliente %s:%d rejeitado: PSK incorreta.\n",
			   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
		close(client_socket);
		return NULL;
	}

	pthread_mutex_lock(&config_mutex);
	struct ConfigMessage config_to_send = current_config;
	pthread_mutex_unlock(&config_mutex);
	if (send(client_socket, &config_to_send, sizeof(config_to_send), 0) < 0) {
		perror("[-] Erro: Falha ao enviar configuração por TCP");
	} else {
		printf("[+] Configuração enviada ao cliente %s:%d por TCP.\n",
			   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
	}

	struct ConfigMessage new_config;
	while (recv(client_socket, &new_config, sizeof(new_config), 0) > 0) {
		pthread_mutex_lock(&config_mutex);
		current_config = new_config;
		pthread_mutex_unlock(&config_mutex);
		printf("[!] Cliente %s:%d pede mudança de configuração.\n",
			   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
		printf("      Retransmissão: %d | Backoff: %d | Sequência: %d | "
			   "Timeout: %dms | Tentativas: %d\n",
			   new_config.enable_retransmission, new_config.enable_backoff,
			   new_config.enable_sequence, new_config.base_timeout_ms,
			   new_config.max_retries);

		if (sendto(multicast_socket, &new_config, sizeof(new_config), 0,
				   (struct sockaddr*)&mcast_addr, sizeof(mcast_addr)) < 0) {
			perror("[-] Erro: Falha ao enviar configuração por Multicast");
		} else {
			printf("[+] Configuração enviada à rede por Multicast.\n");
		}
	}

	printf("[-] Cliente %s:%d desconectado.\n", inet_ntoa(client_addr.sin_addr),
		   ntohs(client_addr.sin_port));
	close(client_socket);
	return NULL;
}

int main(int argc, char* argv[]) {
	if (argc != 5) {
		printf("Uso: %s <IP_SERVIDOR> <PORTO_SERVIDOR> <IP_MULTICAST> "
			   "<PORTO_MULTICAST>",
			   argv[0]);
		return 1;
	}

	const char* server_ip = argv[1];
	int server_port = atoi(argv[2]);
	const char* multicast_ip = argv[3];
	int multicast_port = atoi(argv[4]);

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
	mcast_addr.sin_addr.s_addr = inet_addr(multicast_ip);
	mcast_addr.sin_port = htons(multicast_port);

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
	localInterface.s_addr = inet_addr(server_ip);
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
	server_addr.sin_addr.s_addr = INADDR_ANY;
	server_addr.sin_port = htons(server_port);

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
	printf("[+] Ouvido registros TCP no porto %d...\n", server_port);

	signal(SIGINT, handle_sigint);
	while (running) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(tcp_server_socket, &fds);

		// Esperar por mensagens apenas por 1 segundo, para de vez em quando
		// verificar se ainda é preciso correr o loop abaixo
		struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
		int ready = select(tcp_server_socket + 1, &fds, NULL, NULL, &timeout);
		if (ready < 0) {
			if (!running) break;
			continue;
		}
		if (ready == 0) continue;

		client_socket =
		accept(tcp_server_socket, (struct sockaddr*)&client_addr, &client_len);
		if (client_socket < 0) continue;

		printf("[+] Nova conexão TCP desde %s:%d\n",
			   inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

		struct ClientArgs* arg = malloc(sizeof(struct ClientArgs));
		arg->socket = client_socket;
		arg->addr = client_addr;
		pthread_t thread;
		pthread_create(&thread, NULL, handle_client, arg);
		pthread_detach(thread);
	}

	printf("\n[*] Fechando servidor\n");
	close(tcp_server_socket);
	close(multicast_socket);
	pthread_mutex_destroy(&config_mutex);
	printf("[+] Servidor fechado corretamente\n");
	return 0;
}
