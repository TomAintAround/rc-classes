#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <pthread.h>
#include <errno.h> // IWYU pragma: keep
#include "powerudp.h"

#define MAX_CONNECTIONS 16

struct SendMessageArgs {
	char dest_ip[16];
	int dest_port;
	char buffer[65535 - sizeof(struct PowerUDP_Header)];
	size_t len;
};

// Estrutura de dados para cada conexão PowerUDP entre 2 clientes
struct Connection {
	char ip[16];
	int port;

	pthread_mutex_t send_mutex;
	pthread_mutex_t ack_mutex;
	pthread_cond_t ack_cond;
	struct PowerUDP_Header pending_ack;
	int has_pending_ack;

	pthread_mutex_t sequence_mutex;
	uint16_t send_sequence_number;
	uint16_t receive_sequence_number;
};

pthread_mutex_t connections_mutex = PTHREAD_MUTEX_INITIALIZER;
struct Connection connections[MAX_CONNECTIONS];
int num_connections = 0;

int tcp_socket = -1;
int udp_socket = -1;
int multicast_socket = -1;

volatile sig_atomic_t running = 1;

pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;
struct ConfigMessage current_config = { .enable_retransmission = 0,
										.enable_backoff = 0,
										.enable_sequence = 0,
										.base_timeout_ms = 1000,
										.max_retries = 3 };

// Estatística
pthread_mutex_t statistics_mutex = PTHREAD_MUTEX_INITIALIZER;
int total_retransmissions = 0;
int last_delivery_time_ms = 0;

// Geral
pthread_mutex_t general_mutex = PTHREAD_MUTEX_INITIALIZER;
int packet_loss_probability = 0;

struct Connection* get_or_create_connection(const char* ip, int port) {
	pthread_mutex_lock(&connections_mutex);

	for (int i = 0; i < num_connections; i++) {
		if (strncmp(connections[i].ip, ip, 16) == 0 && connections[i].port == port) {
			pthread_mutex_unlock(&connections_mutex);
			return &connections[i];
		}
	}

	if (num_connections >= MAX_CONNECTIONS) {
		printf("[-] Limite de conexões atingido.\n");
		pthread_mutex_unlock(&connections_mutex);
		return NULL;
	}

	struct Connection* connection = &connections[num_connections++];
	strncpy(connection->ip, ip, 16);
	connection->port = port;
	connection->send_sequence_number = 0;
	connection->receive_sequence_number = 0;
	connection->has_pending_ack = 0;
	pthread_mutex_init(&connection->send_mutex, NULL);
	pthread_mutex_init(&connection->ack_mutex, NULL);
	pthread_mutex_init(&connection->sequence_mutex, NULL);
	pthread_cond_init(&connection->ack_cond, NULL);

	printf("[+] Nova conexão registada: %s:%d\n", ip, port);
	pthread_mutex_unlock(&connections_mutex);
	return connection;
}

int init_protocol(const char* server_ip, int server_port, int my_port,
				  const char* multicast_ip, int multicast_port, const char* psk) {
	struct sockaddr_in server_addr;
	struct RegisterMessage reg_msg;
	struct hostent* host_ptr;

	// TCP
	if ((host_ptr = gethostbyname(server_ip)) == 0) {
		perror("[-] Erro: Não consegui obter endereço");
		return -1;
	}

	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(server_port);
	server_addr.sin_addr.s_addr = ((struct in_addr*)(host_ptr->h_addr))->s_addr;

	tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (tcp_socket < 0) {
		perror("[-] Erro: Não foi possível criar o socket TCP");
		return -1;
	}

	if (connect(tcp_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
		perror("[-] Erro: Não possível realizar a conexão TCP com o servidor");
		close(tcp_socket);
		return -1;
	}

	strncpy(reg_msg.psk, psk, sizeof(reg_msg.psk) - 1);
	reg_msg.psk[sizeof(reg_msg.psk) - 1] = '\0'; // Assegurar que o final tem \0

	if (send(tcp_socket, &reg_msg, sizeof(reg_msg), 0) < 0) {
		perror("[-] Erro: Não foi possível enviar a mensagem de registro");
		close(tcp_socket);
		return -1;
	}

	if (recv(tcp_socket, &reg_msg, 1, 0) <= 0) {
		perror("[-] Erro: Confirmação de registro do servidor");
		close(tcp_socket);
		exit(1);
	}

	if (reg_msg.psk[0] == 0) {
		printf(
		"[-] Erro: Registro TCP enviado ao servidor com chave errada.\n");
		close(tcp_socket);
		exit(1);
	}

	printf("[+] Registro TCP enviado ao servidor com êxito.\n");

	struct ConfigMessage new_config;
	if (recv(tcp_socket, &new_config, sizeof(new_config), 0) < 0) {
		perror("[-] Erro: Receber configuração do servidor\n");
		close(tcp_socket);
		exit(1);
	}
	pthread_mutex_lock(&config_mutex);
	current_config = new_config;
	pthread_mutex_unlock(&config_mutex);

	printf("[!] ATENÇÃO: Configuração aplicada desde o Servidor\n");
	printf("      Retransmissão: %d | Backoff: %d | Sequência: %d | Timeout: "
		   "%dms | Tentativas: %d\n",
		   new_config.enable_retransmission, new_config.enable_backoff,
		   new_config.enable_sequence, new_config.base_timeout_ms, new_config.max_retries);

	// UDP
	udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
	if (udp_socket < 0) {
		perror("[-] Erro: Não foi possível criar o socket UDP");
		close(tcp_socket);
		return -1;
	}
	printf("[+] Socket UDP (PowerUDP) criado e preparado.\n");

	struct sockaddr_in local_addr;
	memset(&local_addr, 0, sizeof(local_addr));
	local_addr.sin_family = AF_INET;
	local_addr.sin_addr.s_addr = INADDR_ANY;
	local_addr.sin_port = htons(my_port);

	if (bind(udp_socket, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
		perror(
		"[-] Erro: Erro ao fazer bind no socket UDP. Possivelmente ocupado");
		return 1;
	}

	// Multicast
	multicast_socket = socket(AF_INET, SOCK_DGRAM, 0);
	if (multicast_socket < 0) {
		perror("[-] Error: No se pudo crear el socket Multicast");
		close(tcp_socket);
		close(udp_socket);
		return -1;
	}

	// Permitir reutilizar o porto para que varios clientes no colidem se estiverem na mesma máquina
	int reuse = 1;
	setsockopt(multicast_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	struct sockaddr_in mcast_addr;
	memset(&mcast_addr, 0, sizeof(mcast_addr));
	mcast_addr.sin_family = AF_INET;
	mcast_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	mcast_addr.sin_port = htons(multicast_port);

	if (bind(multicast_socket, (struct sockaddr*)&mcast_addr, sizeof(mcast_addr)) < 0) {
		perror("[-] Erro: Erro ao fazer bind no socket Multicast");
		close(tcp_socket);
		close(udp_socket);
		close(multicast_socket);
		return -1;
	}

	struct ip_mreq mreq;
	mreq.imr_multiaddr.s_addr = inet_addr(multicast_ip);
	mreq.imr_interface.s_addr = htonl(INADDR_ANY);

	if (setsockopt(multicast_socket, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
		perror("[-] Erro: Não foi possível unir ao grupo Multicast");
		close(tcp_socket);
		close(udp_socket);
		close(multicast_socket);
		return -1;
	}
	printf("[+] Unido ao grupo Multicast %s:%d con êxito.\n", multicast_ip, multicast_port);

	return 0;
}

int send_message(const char* dest_ip, int dest_port, const char* message, size_t len) {
	struct Connection* conn = get_or_create_connection(dest_ip, dest_port);
	if (!conn) return -1;

	pthread_mutex_lock(&conn->send_mutex); // só uma mensagem em trânsito de cada vez

	size_t packet_size = sizeof(struct PowerUDP_Header) + len;
	char* packet = malloc(packet_size);

	struct PowerUDP_Header* header = (struct PowerUDP_Header*)packet;
	header->type = TYPE_DATA;
	header->payload_length = len;
	memcpy(packet + sizeof(struct PowerUDP_Header), message, len);

	pthread_mutex_lock(&config_mutex);
	int max_retries = current_config.max_retries;
	bool enable_retransmit = current_config.enable_retransmission;
	bool enable_backoff = current_config.enable_backoff;
	bool enable_sequence = current_config.enable_sequence;
	long base_timeout = current_config.base_timeout_ms;
	pthread_mutex_unlock(&config_mutex);

	pthread_mutex_lock(&conn->sequence_mutex);
	header->sequence_number = conn->send_sequence_number;
	pthread_mutex_unlock(&conn->sequence_mutex);

	struct sockaddr_in dest_addr;
	memset(&dest_addr, 0, sizeof(dest_addr));
	dest_addr.sin_family = AF_INET;
	dest_addr.sin_port = htons(dest_port);
	inet_pton(AF_INET, dest_ip, &dest_addr.sin_addr);

	int attempts = 0;
	bool ack_received = false;
	long current_timeout = base_timeout;
	int result = -1;

	struct timespec start, end;
	while (!ack_received) {
		// Simular perda de pacotes
		pthread_mutex_lock(&general_mutex);
		int curr_loss_prob = packet_loss_probability;
		pthread_mutex_unlock(&general_mutex);

		if (rand() % 100 >= curr_loss_prob) {
			sendto(udp_socket, packet, packet_size, 0,
				   (struct sockaddr*)&dest_addr, sizeof(dest_addr));
		} else {
			printf("[-] [SIMULAÇÃO] Pacote largado de propósito.\n");
		}
		clock_gettime(CLOCK_REALTIME, &start);

		if (!enable_retransmit) {
			result = 0;
			break;
		}

		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_sec += current_timeout / 1000;
		ts.tv_nsec += (current_timeout % 1000) * 1000000;
		if (ts.tv_nsec >= 1000000000) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000;
		}

		// Verificar se ainda há um pacote com ACK pendente
		pthread_mutex_lock(&conn->ack_mutex);
		while (!conn->has_pending_ack) {
			int rc = pthread_cond_timedwait(&conn->ack_cond, &conn->ack_mutex, &ts);
			if (rc == ETIMEDOUT) break;
		}

		if (conn->has_pending_ack) {
			struct PowerUDP_Header ack = conn->pending_ack;
			conn->has_pending_ack = 0;
			pthread_mutex_unlock(&conn->ack_mutex);

			if (ack.type == TYPE_ACK && ack.sequence_number == header->sequence_number) {
				printf("[+] ACK recebido para o pacote %d\n", ack.sequence_number);
				ack_received = true;
				result = 0;
			} else if (ack.type == TYPE_NACK) {
				printf("[-] NACK recebido para o pacote %d\n", ack.sequence_number);
				result = -1;
				break;
			}
		} else {
			pthread_mutex_unlock(&conn->ack_mutex);

			attempts++;
			pthread_mutex_lock(&statistics_mutex);
			total_retransmissions++;
			pthread_mutex_unlock(&statistics_mutex);
			printf("[-] Timeout. Tentativa %d de %d.\n", attempts, max_retries);

			if (attempts >= max_retries) {
				printf("[!] Limite de retransmissões atingido.\n");
				result = -1;
				break;
			}

			if (enable_backoff)
				current_timeout = base_timeout * (1 << attempts);
		}
	}
	clock_gettime(CLOCK_REALTIME, &end);

	pthread_mutex_lock(&statistics_mutex);
	last_delivery_time_ms = (int)(((end.tv_sec - start.tv_sec) * 1000) +
								  ((end.tv_nsec - start.tv_nsec) / 1000000));
	pthread_mutex_unlock(&statistics_mutex);

	free(packet);

	if (result == 0 && enable_sequence) {
		pthread_mutex_lock(&conn->sequence_mutex);
		conn->send_sequence_number++;
		pthread_mutex_unlock(&conn->sequence_mutex);
	}

	pthread_mutex_unlock(&conn->send_mutex);
	return result;
}

void* send_message_thread(void* arg) {
	struct SendMessageArgs* args = (struct SendMessageArgs*)arg;
	send_message(args->dest_ip, args->dest_port, args->buffer, args->len);
	free(arg);
	return NULL;
}

int receive_message(char* buffer, int bufsize) {
	char temp_packet[65535];
	struct sockaddr_in sender_addr;
	socklen_t sender_len = sizeof(sender_addr);

	int bytes_received = (int)recvfrom(udp_socket, temp_packet, sizeof(temp_packet),
									   0, (struct sockaddr*)&sender_addr, &sender_len);

	if (bytes_received < 0) {
		perror("[-] Erro ao receber mensagem UDP");
		return -1;
	}

	if (bytes_received < (int)sizeof(struct PowerUDP_Header)) {
		printf("[-] Pacote descartado: demasiado pequeno.\n");
		return -1;
	}

	struct PowerUDP_Header* header = (struct PowerUDP_Header*)temp_packet;

	char sender_ip[16];
	inet_ntop(AF_INET, &sender_addr.sin_addr, sender_ip, sizeof(sender_ip));
	int sender_port = ntohs(sender_addr.sin_port);

	struct Connection* conn = get_or_create_connection(sender_ip, sender_port);
	if (!conn) {
		printf("[-] Pacote descartado: limite de conexões atingido.\n");
		return -1;
	}

	if (header->type == TYPE_ACK || header->type == TYPE_NACK) {
		pthread_mutex_lock(&conn->ack_mutex);
		conn->pending_ack = *header;
		conn->has_pending_ack = 1;
		pthread_cond_signal(&conn->ack_cond);
		pthread_mutex_unlock(&conn->ack_mutex);
		return 0;
	}

	if (header->type != TYPE_DATA) {
		printf("[-] Pacote descartado: tipo desconhecido.\n");
		return -1;
	}

	pthread_mutex_lock(&config_mutex);
	bool enable_retransmit = current_config.enable_retransmission;
	bool enable_sequence = current_config.enable_sequence;
	pthread_mutex_unlock(&config_mutex);

	struct PowerUDP_Header response;
	response.payload_length = 0;
	response.sequence_number = header->sequence_number;

	if (enable_sequence) {
		pthread_mutex_lock(&conn->sequence_mutex);
		uint16_t expected = conn->receive_sequence_number;
		pthread_mutex_unlock(&conn->sequence_mutex);

		if (header->sequence_number != expected) {
			printf("[-] Fora de ordem (esperado: %d, recebido: %d).\n",
				   expected, header->sequence_number);
			response.type = TYPE_NACK;
			if (enable_retransmit)
				sendto(udp_socket, &response, sizeof(response), 0,
					   (struct sockaddr*)&sender_addr, sender_len);
			printf("[-] NACK enviado.\n");
			return 0;
		}
	}

	int data_len = header->payload_length;
	if (data_len > bufsize) data_len = bufsize;
	memcpy(buffer, temp_packet + sizeof(struct PowerUDP_Header), data_len);

	if (enable_sequence) {
		pthread_mutex_lock(&conn->sequence_mutex);
		conn->receive_sequence_number++;
		pthread_mutex_unlock(&conn->sequence_mutex);
	}

	response.type = TYPE_ACK;
	if (enable_retransmit)
		sendto(udp_socket, &response, sizeof(response), 0,
			   (struct sockaddr*)&sender_addr, sender_len);

	printf("[+] Pacote %d recebido corretamente.\n", header->sequence_number);
	return data_len;
}

void* receive_message_thread(void* arg) {
	(void)arg;
	char receive_buffer[65535 - sizeof(struct PowerUDP_Header)];
	while (running) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(udp_socket, &fds);
		struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };

		if (select(udp_socket + 1, &fds, NULL, NULL, &timeout)) {
			int bytes = receive_message(receive_buffer, sizeof(receive_buffer));
			if (bytes > 0)
				printf("\n[MENSAGEM RECEBIDA]: %s\n", receive_buffer);
		}
	}
	return NULL;
}

int request_protocol_config(int enable_retransmission, int enable_backoff, int enable_sequence,
							uint16_t base_timeout, uint8_t max_retries) {
	if (tcp_socket < 0) {
		printf("[-] Erro: No há conexão TCP ativa com o servidor.\n");
		return -1;
	}

	struct ConfigMessage new_config = {
		.enable_retransmission = enable_retransmission,
		.enable_backoff = enable_backoff,
		.enable_sequence = enable_sequence,
		.base_timeout_ms = base_timeout,
		.max_retries = max_retries,
	};

	if (send(tcp_socket, &new_config, sizeof(new_config), 0) < 0) {
		perror("[-] Erro ao enviar o pedido de mudança de configuração");
		return -1;
	}

	printf("[+] Pedido de mudança de configuração enviada ao servidor.\n");
	return 0;
}

int get_last_message_stats(int* retransmissions, int* delivery_time) {
	pthread_mutex_lock(&statistics_mutex);
	if (retransmissions != NULL) *retransmissions = total_retransmissions;
	if (delivery_time != NULL) *delivery_time = last_delivery_time_ms;
	pthread_mutex_unlock(&statistics_mutex);
	return 0;
}

void inject_packet_loss(int probability) {
	if (probability < 0) probability = 0;
	if (probability > 100) probability = 100;

	pthread_mutex_lock(&general_mutex);
	packet_loss_probability = probability;
	pthread_mutex_unlock(&general_mutex);
	printf("[!] Probabilidade de perda de pacotes configurada a %d%%\n", probability);
}

void* multicast_thread(void* arg) {
	(void)arg;
	struct ConfigMessage new_config;
	struct sockaddr_in server_addr;
	socklen_t srv_len = sizeof(server_addr);

	while (running) {
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(multicast_socket, &fds);
		struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };

		if (select(multicast_socket + 1, &fds, NULL, NULL, &timeout)) {
			if (recvfrom(multicast_socket, &new_config, sizeof(new_config), 0,
						 (struct sockaddr*)&server_addr, &srv_len) > 0) {
				pthread_mutex_lock(&config_mutex);
				current_config = new_config;
				pthread_mutex_unlock(&config_mutex);
				printf("[!!!] ATENÇÃO: Nova configuração aplicada [!!!]\n");
				printf("      Retransmissão: %d | Backoff: %d | Sequência: %d "
					   "| Timeout: %dms | Tentativas: %d\n",
					   new_config.enable_retransmission,
					   new_config.enable_backoff, new_config.enable_sequence,
					   new_config.base_timeout_ms, new_config.max_retries);
			}
		}
	}
	return NULL;
}

void close_protocol() {
	printf("[*] Fechando conexões de rede...\n");

	if (tcp_socket >= 0) {
		close(tcp_socket);
		tcp_socket = -1;
	}
	if (udp_socket >= 0) {
		close(udp_socket);
		udp_socket = -1;
	}
	if (multicast_socket >= 0) {
		close(multicast_socket);
		multicast_socket = -1;
	}

	pthread_mutex_destroy(&config_mutex);
	pthread_mutex_destroy(&statistics_mutex);
	pthread_mutex_destroy(&general_mutex);
	for (int i = 0; i < num_connections; i++) {
		pthread_mutex_destroy(&connections[i].send_mutex);
		pthread_mutex_destroy(&connections[i].ack_mutex);
		pthread_mutex_destroy(&connections[i].sequence_mutex);
		pthread_cond_destroy(&connections[i].ack_cond);
	}

	printf("[+] Protocolo PowerUDP fechado corretamente.\n");
}

int main(int argc, char* argv[]) {
	if (argc != 7) {
		printf("Uso: %s <IP_SERVIDOR> <PORTO_SERVIDOR> <MEU_PORTO_UDP> "
			   "<IP_MULTICAST> <PORTO_MULTICAST> <PALAVRA_PASSE>\n",
			   argv[0]);
		return 1;
	}

	const char* server_ip = argv[1];
	int server_port = atoi(argv[2]);
	int my_port = atoi(argv[3]);
	const char* multicast_ip = argv[4];
	int multicast_port = atoi(argv[5]);
	const char* psk = argv[6];

	if (init_protocol(server_ip, server_port, my_port, multicast_ip,
					  multicast_port, psk) < 0) {
		printf("[-] Erro fatal ao iniciar. Saindo...\n");
		return 1;
	}

	printf("[+] Socket UDP (PowerUDP) ligado localmente ao porto %d.\n", my_port);
	printf("\n======================================================\n");
	printf(" Cliente PowerUDP Iniciado.\n");
	printf(" Conectado ao Servidor: %s:%d\n", server_ip, server_port);
	printf(" Comandos disponíveis:\n");
	printf("  Alterar configuração:\n");
	printf("   /config <retrans> <backoff> <seq> <timeout> <retries>\n");
	printf("  Alterar probabilidade de não enviar pacote de propósito\n");
	printf("  (por motivos de simulação):");
	printf("   /drop <probabilidad_0_a_100>\n");
	printf("  Ver estatísticas:\n");
	printf("   /stats\n");
	printf("  Terminar programa:\n");
	printf("   /quit\n");
	printf(" Escreve uma mensagem com o seguinte formato e clica\n");
	printf(" ENTER para enviar uma mensagem com PowerUDP:\n");
	printf("<ip_destino> <porto_destino> <mensagem>\n");
	printf("======================================================\n\n");

	pthread_t receive_thread, mcast_thread;
	pthread_create(&receive_thread, NULL, receive_message_thread, NULL);
	pthread_create(&mcast_thread, NULL, multicast_thread, NULL);

	char input_buffer[65535];
	while (running) {
		fgets(input_buffer, sizeof(input_buffer), stdin);
		input_buffer[strcspn(input_buffer, "\n")] = '\0'; // Tirar o \n

		if (strncmp(input_buffer, "/quit", 5) == 0) {
			running = 0;
			break;
		}

		if (strncmp(input_buffer, "/drop", 5) == 0) {
			int prob;
			if (sscanf(input_buffer, "/drop %d", &prob) == 1) {
				inject_packet_loss(prob);
			} else {
				printf("[-] Uso: /drop <probabilidade>\n");
			}
		} else if (strncmp(input_buffer, "/stats", 6) == 0) {
			int retrans, time;
			get_last_message_stats(&retrans, &time);
			printf("[i] Estatísticas globais:\n");
			printf("     Retransmissões totais: %d\n", retrans);
			printf("     Último tempo de resposta (ms): %d\n", time);
		} else if (strncmp(input_buffer, "/config", 7) == 0) {
			int retrans, backoff, seq, timeout, retries;
			if (sscanf(input_buffer, "/config %d %d %d %d %d", &retrans,
					   &backoff, &seq, &timeout, &retries) == 5) {
				request_protocol_config(retrans, backoff, seq, timeout, retries);
			} else {
				printf("[-] Uso: /config <retrans> <backoff> <seq> "
					   "<timeout> <retries>\n");
				printf("    Ex.:  /config 1 1 1 1000 5\n");
			}
		} else if (strlen(input_buffer) > 0) {
			char dest_ip[16];
			int dest_port;
			char message[65535 - sizeof(struct PowerUDP_Header)];
			// string integer string(com espaços)
			if (sscanf(input_buffer, "%s %d %[^\n]", dest_ip, &dest_port, message) != 3) {
				printf("[-] Uso: <ip_destino> <porto_destino> <mensagem>\n");
				printf("    Ex.:  193.137.101.3 80 Escuta?\n");
				continue;
			}

			printf("[TU]: Enviando...\n");

			struct SendMessageArgs* args = malloc(sizeof(struct SendMessageArgs));
			strncpy(args->dest_ip, dest_ip, 16);
			args->dest_port = dest_port;
			strncpy(args->buffer, message, sizeof(args->buffer));
			args->len = strlen(message) + 1;

			pthread_t send;
			pthread_create(&send, NULL, send_message_thread, args);
			pthread_detach(send);
		}
	}

	pthread_join(receive_thread, NULL); // espera que terminem limpo
	pthread_join(mcast_thread, NULL);
	close_protocol();
	return 0;
}
