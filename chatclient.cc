#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "tokenizer.h"

#include <cerrno>
#include <iostream>
#include <string>
#include <vector>

#define MAX_CMD_LENGTH 1000

using namespace std;

struct sockaddr_in SERVER_ADDR;
int SERVER_SOCKET;

// Set sig handler that handles CLTR+C Correctly
void sig_handler(int signo) {
    // Upon receiving SIGINT we send a quit command so the server can remove us from
    // Any current groups, 
    if (signo == SIGINT) {
        string quit_cmd = "/quit";
        sendto(SERVER_SOCKET, quit_cmd.c_str(), quit_cmd.size(), 0, (struct sockaddr*)&SERVER_ADDR, sizeof(SERVER_ADDR));
        cout << "" << endl;
        close(SERVER_SOCKET);
        exit(EXIT_SUCCESS);
    }
}

int main(int argc, char *argv[]) {
    if (signal(SIGINT, sig_handler) == SIG_ERR) {
        perror("Error: Unable to catch SIGINT.");
        exit(EXIT_FAILURE);
    }

    if (argc != 2) {
        fprintf(stderr, "*** Author: Leon Hertzberg (leonjh)\n");
        exit(EXIT_SUCCESS);
    }

    int sock = socket(PF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        fprintf(stderr, "Cannot open socket (%s)\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        fprintf(stderr, "Cannot set socket options (%s)\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Get the input given to the client for setting up the client
    vector<string> parsed_input;
    split_tokens_given_token(parsed_input, argv[1], strlen(argv[1]), ":");

    // Initialize the sockaddr_in for the server we're connected to
    // following the same procedue as lecture
    struct sockaddr_in dest;
    bzero(&dest, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(stoi(parsed_input[1]));
    inet_pton(AF_INET, parsed_input[0].c_str(), &dest.sin_addr);

    // Initialize global variables
    SERVER_ADDR = dest;
    SERVER_SOCKET = sock;

    // Getting the source address
    // We need this later for receiving messages (see slides)
    struct sockaddr_in src;
    socklen_t src_size = sizeof(src);

    while (1) {
        // The buffer which will be read into depending on the source
        char buffer[MAX_CMD_LENGTH] = {0};
        if (true) {
        }

        // The fdset which we possibly read from - either STDIN or the socket
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(STDIN_FILENO, &read_set);
        FD_SET(sock, &read_set);

        // Use select to poll if either of the file descriptors have data ready
        int ready_fd = select(sock + 1, &read_set, NULL, NULL, NULL);

        if (FD_ISSET(STDIN_FILENO, &read_set)) {
            // When receiving from STDIN we need to remove any trailing
            // <CR> <LF> or <CRLF>
            int num_read = read(STDIN_FILENO, buffer, MAX_CMD_LENGTH);
            if (num_read < 0) {
                // fprintf(stderr, "Error reading from STDIN (%s)\n",
                // strerror(errno)); exit(EXIT_FAILURE);
                continue; // If nothing is received just re-loop since this is
                          // UDP
            }

            // Remove any trailer <CRLF>, <CR>, or <LF>.
            if (buffer[num_read - 1] == '\r') {
                buffer[num_read - 1] = 0;
            } else if (buffer[num_read - 1] == '\n') {
                buffer[num_read - 1] = 0;
            } else if (num_read > 2 && buffer[num_read - 2] == '\r' &&
                       buffer[num_read - 1] == '\n') {
                buffer[num_read - 2] = 0;
                buffer[num_read - 1] = 0;
            }

            // Since data has been received & pruned, we send it to the server
            // as a datagram
            int send_to_ret =
                sendto(sock, buffer, strlen(buffer), 0, (struct sockaddr *)&dest, sizeof(dest));
            if (send_to_ret < 0) {
                fprintf(stderr, "Error during sendto() (%s)\n", strerror(errno));
                exit(EXIT_FAILURE);
            }

            // If the client entered quit exit the program - note per edstem commands 
            // are case sensitive.
            if (strcmp(buffer, "/quit") == 0) {
                exit(EXIT_SUCCESS);
            }
        } else if (FD_ISSET(sock, &read_set)) {
            // Read the data sent to the socket
            int recv_from_ret =
                recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&src, &src_size);

            // If we fail the request just continue - possible with datagrams
            // Unsure if this is needed - we might want to have a different
            // result (i.e. a print)
            if (recv_from_ret < 0) {
                continue;
            }

            // Upon successfully receiving data from the socket print it to the
            // terminal
            fprintf(stdout, "%s\n", buffer); // Might just want to use write
        }
    }

    close(sock);
    return 0;
}
