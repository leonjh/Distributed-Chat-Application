#pragma once

#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <string>

using namespace std;

#define MAX_LINE_LENGTH 4096 // The max line length we allow - not used but save incase necessary
#define MAX_CMD_LENGTH 1000  // The maximum length of a command possible per the writeup
#define MAX_NUM_GROUPS 50    // The maximum number of groups allowed
#define MAX_NUM_SERVERS 20   // The maximum number of servers allowed
#define INVALID_GROUP -1     // Placeholder for invalid group before a client joins / after leaving.

/**
 * @brief Our logging function used throughout the program - inspired by logVerbose in the test suite
 * This function takes in any number of arguments and prints them in the format
 * 'CURRENT_TIME PASSED_IN_MESSAGE\n'
 * 
 */
#define log_message(a...)                                                                          \
    do {                                                                                           \
        if (VERBOSE) {                                                                             \
            struct timeval tv;                                                                     \
            gettimeofday(&tv, NULL);                                                               \
            struct tm *info;                                                                       \
            info = localtime(&tv.tv_sec);                                                          \
            char formatted_time[80] = {0};                                                         \
            char final_buffer[MAX_LINE_LENGTH] = {0};                                              \
            strftime(formatted_time, sizeof(formatted_time), "%H:%M:%S", info);                    \
            snprintf(final_buffer, MAX_LINE_LENGTH, "%s.%06ld", formatted_time, tv.tv_usec);       \
            printf("%s ", final_buffer);                                                           \
            printf(a);                                                                             \
            printf("\n");                                                                          \
        }                                                                                          \
    } while (0)

/**
 * @brief enums representing which stage of total ordering we're currently in, 
 * plus NOT_TOTAL_ORDER for when we need to pass an argument but we're not using total 
 * ordering,
 * 
 */
typedef enum total_order_status_enum {
    NOT_TOTAL_ORDER,   // We're not using total ordering so ignore - placeholder
    COLLECT_PROPOSALS, // Corresponds to step 2 from the slides
    PROCESS_PROPOSALS, // Corresponds to steps 3 from the slides
    DELIVER_MESSAGES,  // Corresponds to step 4 & 5 from the slides
} total_order_status;

/**
 * @brief enums representing which form of ordering the server was initialized with
 * 
 */
typedef enum order_type_enum {
    UNORDERED_ORDERING,
    FIFO_ORDERING,
    TOTAL_ORDERING,
} order_type;

/**
 * @brief A struct representing a currently connected client and all associated info
 * 
 */
struct client_t {
    sockaddr_in address; // The sockaddr_in from which the client sends us info
    socklen_t address_size; // The sockaddr_in size so we don't have to continually calculate it 
    string nick; // The nickname the client provided in /nick
    int group; // The group the client is currently a member of
    
    // Overload the equals operator so that clients are equals if they have the same originating info (ip/port) & group
    bool operator==(const client_t &other) const {
        return address.sin_addr.s_addr == other.address.sin_addr.s_addr && address.sin_port == other.address.sin_port && group == other.group;
    }
};

struct message_t {
    int sequence_number;
    int sender_id;
    bool is_deliverable;
    string UUID;
    string message;

    // Overload the less than operator so that a server is considered less if a) the sequence number is lower, or b) sequence number is equal but server id is lower
    bool operator<(const message_t &other) const {
        return sequence_number < other.sequence_number || (sequence_number == other.sequence_number && sender_id < other.sender_id);
    }

    // Overload the greater than operator so that a server is considered greater if a) the sequence number is higher, or b) sequence number is equal but server id is greater
    bool operator>(const message_t &other) const {
        return sequence_number > other.sequence_number || (sequence_number == other.sequence_number && sender_id > other.sender_id);
    }

    // Overload the equals operator so that a server is considered equal if the contained messages and UUID's are equal
    bool operator==(const message_t &other) const {
        return UUID.compare(other.UUID) == 0 && message.compare(other.message) == 0;
    }
};
