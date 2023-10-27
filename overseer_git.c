#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_COMPONENTS 100
#define MAX_FLOORS 10
#define AUTHORISATION_FILE "authorisation.txt"
#define CONNECTIONS_FILE "connections.txt"
#define ELEVATOR_CONTROLLER_IP "127.0.0.1"
#define ELEVATOR_CONTROLLER_PORT 5000
#define MAX_TEMPERATURE_SENSORS 10
#define NUM_DOORS 10
#define MAX_CONNECTIONS 10
#define MAX_CODE_LENGTH 20


struct component {
    char type[256];
    char id[256];
    char address[256];
    char port[256];
    char mode[256];
};


struct temperature_sensor {
    int id;
    float value;
    char timestamp[20];
};

struct connection {
    int id;
    int door_id;
};

struct connection connections[MAX_CONNECTIONS] = {
    {1, 2},
    {2, 3},
    // ...
};

struct temperature_sensor temperature_sensors[MAX_TEMPERATURE_SENSORS];


struct component components[MAX_COMPONENTS];
int num_components = 0;
int num_temperature_sensors;

void update_temperature_sensor(int id, double value, const char* timestamp) {
    // Find temperature sensor with matching ID
    int index = -1;
    for (int i = 0; i < num_temperature_sensors; i++) {
        if (temperature_sensors[i].id == id) {
            index = i;
            break;
        }
    }

    // If temperature sensor not found, add it to the list
    if (index == -1) {
        if (num_temperature_sensors >= MAX_TEMPERATURE_SENSORS) {
            fprintf(stderr, "Maximum number of temperature sensors reached\n");
            return;
        }
        index = num_temperature_sensors;
        num_temperature_sensors++;
        temperature_sensors[index].id = id;
        temperature_sensors[index].value = value;
        strncpy(temperature_sensors[index].timestamp, timestamp, sizeof(temperature_sensors[index].timestamp));
        return;
    }

    // If temperature sensor found, update its value if the incoming value is more recent
    if (strcmp(timestamp, temperature_sensors[index].timestamp) > 0) {
        temperature_sensors[index].value = value;
        strncpy(temperature_sensors[index].timestamp, timestamp, sizeof(temperature_sensors[index].timestamp));
    }
}

void send_elevator_request(int id, const char* request) {
    // Create socket for elevator controller
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return;
    }

    // Set up address for elevator controller
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ELEVATOR_CONTROLLER_IP);
    addr.sin_port = htons(ELEVATOR_CONTROLLER_PORT + id);

    // Send elevator request to elevator controller
    if (sendto(sock, request, strlen(request), 0, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("sendto");
    }

    // Close socket
    close(sock);
}

int lookup_connection(int id, int door_id) {
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections[i].id == id && connections[i].door_id == door_id) {
            return 1;
        }
    }
    return 0;
}

void* elevator_control(void* arg) {
    int sock = *(int*)arg;

    while (1) {
        // Receive DESTSELECT message
        char buffer[256];
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        if (recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr*)&client_addr, &client_addr_len) <= 0) {
            perror("recvfrom");
            continue;
        }

        // Parse DESTSELECT message
        char type[256], id[256], action[256], scanned[256], floor[256];
        if (sscanf(buffer, "%s %s %s %s %s", type, id, action, scanned, floor) != 5) {
            fprintf(stderr, "Invalid DESTSELECT message: %s\n", buffer);
            continue;
        }

        // Check if message is DESTSELECT
        if (strcmp(type, "DESTSELECT") != 0) {
            fprintf(stderr, "Invalid message type: %s\n", type);
            continue;
        }

        // Get user access floors from authorisation file
        int user_id = atoi(scanned);
        int access_floors[MAX_FLOORS];
        int num_access_floors = 0;
        FILE* auth_file = fopen(AUTHORISATION_FILE, "r");
        if (auth_file == NULL) {
            perror("fopen");
            continue;
        }
        char auth_line[256];
        while (fgets(auth_line, sizeof(auth_line), auth_file) != NULL) {
            int id, floor;
            if (sscanf(auth_line, "%d %d", &id, &floor) == 2 && id == user_id) {
                access_floors[num_access_floors++] = floor;
            }
        }
        fclose(auth_file);

        // Get destination select panel floor and elevator ID from connections file
        int panel_id = atoi(id);
        int panel_floor = -1;
        int elevator_id = -1;
        FILE* conn_file = fopen(CONNECTIONS_FILE, "r");
        if (conn_file == NULL) {
            perror("fopen");
            continue;
        }
        char conn_line[256];
        while (fgets(conn_line, sizeof(conn_line), conn_file) != NULL) {
            int id, floor, elevator;
            if (sscanf(conn_line, "%d %d %d", &id, &floor, &elevator) == 3 && id == panel_id) {
                panel_floor = floor;
                elevator_id = elevator;
                break;
            }
        }
        fclose(conn_file);

        // Check if access is granted
        int access_granted = 0;
        for (int i = 0; i < num_access_floors; i++) {
            if (access_floors[i] == atoi(floor) && access_floors[i] != panel_floor) {
                access_granted = 1;
                break;
            }
        }

        // Send ALLOWED or DENIED message to destination select controller
        char response[256];
        if (access_granted) {
            snprintf(response, sizeof(response), "ALLOWED\n");
            sendto(sock, response, strlen(response), 0, (struct sockaddr*)&client_addr, client_addr_len);

            // Send elevator request to elevator controller
            char request[256];
            snprintf(request, sizeof(request), "FROM %d TO %d\n", panel_floor, atoi(floor));
            send_elevator_request(elevator_id, request);
        } else {
            snprintf(response, sizeof(response), "DENIED\n");
            sendto(sock, response, strlen(response), 0, (struct sockaddr*)&client_addr, client_addr_len);
        }
    }

    return NULL;
}


int connect_to_door(int door_id) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8080 + door_id);
    inet_aton("127.0.0.1", &addr.sin_addr);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return -1;
    }

    return sock;
}


struct door {
    int id;
    char access_code[20];
};

struct door doors[NUM_DOORS] = {
    {1, "1234"},
    {2, "5678"},
    // ...
};

char* get_access_code(int door_id) {
    for (int i = 0; i < NUM_DOORS; i++) {
        if (doors[i].id == door_id) {
            return doors[i].access_code;
        }
    }
    return NULL;
}

int check_authorisation(char* code, char* auth_file) {
    // Open the authorization file for reading
    FILE* fp = fopen(auth_file, "r");
    if (fp == NULL) {
        perror("fopen");
        return 0;
    }

    // Read the authorization codes from the file
    char codes[10][MAX_CODE_LENGTH];
    int num_codes = 0;
    while (fgets(codes[num_codes], MAX_CODE_LENGTH, fp) != NULL) {
        // Remove the newline character from the end of the code
        codes[num_codes][strcspn(codes[num_codes], "\n")] = '\0';
        num_codes++;
    }

    // Close the authorization file
    fclose(fp);

    // Check if the code matches any of the authorization codes
    for (int i = 0; i < num_codes; i++) {
        if (strcmp(code, codes[i]) == 0) {
            return 1;
        }
    }

    // If the code does not match any of the authorization codes, return 0
    return 0;
}

void us_sleep(unsigned int microseconds) {
    struct timespec ts;
    ts.tv_sec = microseconds / 1000000;
    ts.tv_nsec = (microseconds % 1000000) * 1000;
    nanosleep(&ts, NULL);
}

void* tcp_thread(void* arg) {
    int server_sock = *(int*)arg;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    while (1) {
        int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_sock == -1) {
            perror("accept");
            continue;
        }

        // Receive message from client
        char *message;
        if (recv(client_sock, message, sizeof(message), 0) <= 0) {
            perror("recv");
            close(client_sock);
            continue;
        }

        // Parse message
        char *type, *id, *code;
        if (sscanf(message, "%s %s %s", type, id, code) != 3) {
            fprintf(stderr, "Invalid message: %s\n", message);
            close(client_sock);
            continue;
        }

        // Check if message is SCANNED
        if (strcmp(type, "SCANNED") != 0) {
            fprintf(stderr, "Invalid message type: %s\n", type);
            close(client_sock);
            continue;
        }

        // Look up card reader ID in connections file
        char *door_id;
        if (!lookup_connection(id, door_id)) {
            fprintf(stderr, "Unknown card reader ID: %s\n", id);
            close(client_sock);
            continue;
        }

        // Look up scanned code in authorisation file
        if (!check_authorisation(code, door_id)) {
            // Send DENIED message to card reader
            const char* response = "DENIED#";
            send(client_sock, response, strlen(response), 0);
            close(client_sock);
            continue;
        }

        // Send ALLOWED message to card reader
        const char* response = "ALLOWED#";
        send(client_sock, response, strlen(response), 0);
        close(client_sock);

        // Open connection to door controller
        int door_sock = connect_to_door(door_id);
        if (door_sock == -1) {
            fprintf(stderr, "Failed to connect to door controller for door %s\n", door_id);
            continue;
        }

        // Send OPEN message to door controller
        const char* open_message = "OPEN#";
        send(door_sock, open_message, strlen(open_message), 0);

        // Wait for door to open
        char door_response[256];
        if (recv(door_sock, door_response, sizeof(door_response), 0) <= 0) {
            perror("recv");
            close(door_sock);
            continue;
        }
        if (strcmp(door_response, "OPENING#") != 0) {
            fprintf(stderr, "Unexpected response from door controller: %s\n", door_response);
            close(door_sock);
            continue;
        }
        if (recv(door_sock, door_response, sizeof(door_response), 0) <= 0) {
            perror("recv");
            close(door_sock);
            continue;
        }
        if (strcmp(door_response, "OPENED#") != 0) {
            fprintf(stderr, "Unexpected response from door controller: %s\n", door_response);
            close(door_sock);
            continue;
        }

        // Wait for door to close
        usleep(10);

        // Send CLOSE message to door controller
        const char* close_message = "CLOSE#";
        send(door_sock, close_message, strlen(close_message), 0);

        close(door_sock);
    }

    return NULL;
}
void list_temperature_sensors() {
    printf("Temperature sensors:\n");
    for (int i = 0; i < num_temperature_sensors; i++) {
        printf("  Sensor %d: %.2f\n", temperature_sensors[i].id, temperature_sensors[i].value);
    }
}


void* udp_thread(void* arg) {
    int sock = *(int*)arg;

    while (1) {
        // Receive UDP packet
        char buffer[256];
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        if (recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr*)&client_addr, &client_addr_len) <= 0) {
            perror("recvfrom");
            continue;
        }

        // Parse UDP packet
        char type[256], id[256], timestamp[256], value[256];
        if (sscanf(buffer, "%s %s %s %s", type, id, timestamp, value) != 4) {
            fprintf(stderr, "Invalid UDP packet: %s\n", buffer);
            continue;
        }

        // Check if message is TEMPERATURE
        if (strcmp(type, "TEMPERATURE") != 0) {
            fprintf(stderr, "Invalid message type: %s\n", type);
            continue;
        }

        // Update temperature sensor value
        int sensor_id = atoi(id);
        double sensor_value = atof(value);
        update_temperature_sensor(sensor_id, sensor_value, timestamp);
    }

    return NULL;
}



int main(int argc, char *argv[]) {
    if (argc != 8) {
        fprintf(stderr, "Usage: %s <address:port> <door open duration> <datagram resend delay> <authorisation file> <connections file> <layout file> <shared memory path>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Parse command line arguments
    char *address_port = argv[1];
    int door_open_duration = atoi(argv[2]);
    int datagram_resend_delay = atoi(argv[3]);
    const char *auth_file = argv[4]; 
    const char *conn_file = argv[5];
    const char *layout_file = argv[6];
    const char *shm_path = argv[7];

    // Initialize shared memory
    initialize_shared_memory(shm_path, 0);

    // Parse address and port
    char address[256], port[256];
    if (sscanf(address_port, "%[^:]:%s", address, port) != 2) {
        fprintf(stderr, "Invalid address:port format: %s\n", address_port);
        exit(EXIT_FAILURE);
    }

    // Create TCP socket
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock == -1) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Bind to address and port
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(address);
    server_addr.sin_port = htons(atoi(port));
    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections
    if (listen(server_sock, 10) == -1) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    // Create TCP thread
    pthread_t tcp_tid;
    if (pthread_create(&tcp_tid, NULL, tcp_thread, &server_sock) != 0) {
        perror("pthread_create");
        exit(EXIT_FAILURE);
    }

    // Create UDP thread
    pthread_t udp_tid;
    if (pthread_create(&udp_tid, NULL, udp_thread, NULL) != 0) {
        perror("pthread_create");
        exit(EXIT_FAILURE);
    }

    int elevator_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (elevator_sock < 0) {
        perror("socket");
        exit(1);
    }
    struct sockaddr_in elevator_addr;
    elevator_addr.sin_family = AF_INET;
    elevator_addr.sin_addr.s_addr = INADDR_ANY;
    elevator_addr.sin_port = htons(6000);
    if (bind(elevator_sock, (struct sockaddr*)&elevator_addr, sizeof(elevator_addr)) < 0) {
        perror("bind");
        exit(1);
    }

    // Start elevator control thread
    pthread_t elevator_thread;
    if (pthread_create(&elevator_thread, NULL, elevator_control, &elevator_sock) != 0) {
        perror("pthread_create");
        exit(1);
    }

    // Wait for threads to finish
    pthread_join(elevator_thread, NULL);
    pthread_join(tcp_tid, NULL);
    pthread_join(udp_tid, NULL);

    return 0;
}
