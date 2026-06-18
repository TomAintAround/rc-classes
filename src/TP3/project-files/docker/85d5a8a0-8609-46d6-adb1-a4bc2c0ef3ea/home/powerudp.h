#ifndef POWERUDP_H
#define POWERUDP_H

#include <stdint.h>
#include <stddef.h>

struct ConfigMessage {
	uint8_t enable_retransmission; // 0 = Desactivado, 1 = Activado
	uint8_t enable_backoff;		   // 0 = Desactivado, 1 = Activado
	uint8_t enable_sequence;	   // 0 = Desactivado, 1 = Activado
	uint16_t base_timeout_ms;	   // Tempo base para timeouts (ms)
	uint8_t max_retries;		   // Número máximo de retransmisiones
};

struct RegisterMessage {
	char psk[64];
};

#define TYPE_DATA 0
#define TYPE_ACK 1
#define TYPE_NACK 2

struct PowerUDP_Header {
	uint8_t type; // Tipo de mensagem: DATA, ACK o NACK
	uint16_t sequence_number;
	uint16_t payload_length;
};

// Inicia a stack de comunicação e registra-o no servidor
int init_protocol(const char* server_ip, int server_port, int my_port,
				  const char* multicast_ip, int multicast_port, const char* psk);

// Termina a stack de cominucação e apagar o registro no servidor
void close_protocol();

// Pede alteração na configuração do protocolo de servidor
int request_protocol_config(int enable_retransmission, int enable_backoff, int enable_sequence,
							uint16_t base_timeout, uint8_t max_retries);

// Envia uma mensagem UDP
int send_message(const char* dest_ip, int dest_port, const char* message, size_t len);

// Recebe ume mensagem UDP
int receive_message(char* buffer, int bufsize);

// Obtém estatísticas da última mensagem enviada
int get_last_message_stats(int* retransmissions, int* delivery_time);

// Simula a perda de pacotes para testar retransmissões
void inject_packet_loss(int probability);

#endif // POWERUDP_H
