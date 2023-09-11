#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "constants.h"
#include "tokenizer.h"

#include <cerrno>
#include <climits>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;

order_type ORDERING = UNORDERED_ORDERING; // The type of ordering to use
bool VERBOSE = false;                     // Whether we're in debug mode or not

int SELF_SOCKET;                      // The socket we open for this server for global access
struct sockaddr_in SELF_SERVER;       // The sockaddr_in we use for this server so we can case later
struct sockaddr_in SELF_SENDING_ADDR; // The sockaddr_in we initially have before binding to listen
int CONFIGURATION_INDEX =
    0; // The index of the cur server - used for telling which node is sending info
vector<sockaddr_in> all_servers; // All the servers that were opened in the config file
vector<sockaddr_in>
    all_inc_cur;              // All servers in the config file including current
                              // We only use this one for sending individual messages, not forwards
vector<client_t> all_clients; // All the clients currently connected to the server
int message_count;
int msg_count;

// FIFO Ordering Primitives
int S[MAX_NUM_GROUPS + 1];                      // Node N's counter for group G per slides
int R[MAX_NUM_SERVERS + 1][MAX_NUM_GROUPS + 1]; // R^g_n per slides
unordered_map<int, string> F_HOLDBACK[MAX_NUM_SERVERS + 1]
                                     [MAX_NUM_GROUPS + 1]; // The holdback queue per slides

// Total Ordering Primitives
int P[MAX_NUM_GROUPS + 1];
int A[MAX_NUM_GROUPS + 1];
map<message_t, string>
    T_HOLDBACK[MAX_NUM_GROUPS + 1]; // Map <Message, uuid> for the holdback queue. Messages hold
                                    // info on deliverability / string etc. Note that since we've
                                    // overloaded the less than symbol this handles sorting for us
map<string, vector<message_t>>
    T_PROPOSALS; // Used as a queue for the proposals, maps UUID to a vector of messages.

/**
 * @brief Checks if two strings are equal regardless of the casing
 *
 * @param a The first string
 * @param b The second string
 * @return true - Returns true if the strings are equal
 * @return false - Returns false otherwise
 */
bool str_equals(string &a, string &b) {
    // Check the sizes are equal to begin
    if (a.size() != b.size()) {
        return false;
    }

    // Check that each character is equal, ignoring casing
    for (int i = 0; i < a.size(); i++) {
        if (tolower(a[i]) != tolower(b[i])) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Checks if two strings are equal regardless of the casing
 *
 * @param a The first string
 * @param b The second string
 * @return true - Returns true if the strings are equal
 * @return false - Returns false otherwise
 */
bool str_equals_given_str(string &a, const char *str) {
    string b(str);

    // Check the sizes are equal to begin
    if (a.size() != b.size()) {
        return false;
    }

    // Check that each character is equal, ignoring casing
    for (int i = 0; i < a.size(); i++) {
        if (tolower(a[i]) != tolower(b[i])) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Create a forward message string to send to other servers
 *
 * @param group The group the client is currently part of
 * @param message The message being passed on
 * @param msg_id The message id / sequencing number of the message
 * @param total_ordering_stage The stage of total ordering we're currently in
 * @param UUID The UUID of the message in the form SERVER_ID/TIMESTAMP/MESSAGE_COUNT
 * @return string The finished forwarding message
 */
string create_forward_message(int group, char message[], int msg_id, int total_ordering_stage,
                              string UUID) {
    // Create a string and populate it with all necessary information for
    // Format: sender, msg_id (sequence number), group, message, total ordering stage, UUID
    string information_message;

    information_message.append(to_string(CONFIGURATION_INDEX));
    information_message.append("$");
    information_message.append(to_string(msg_id));
    information_message.append("$");
    information_message.append(to_string(group));
    information_message.append("$");
    information_message.append(message);
    information_message.append("$");
    information_message.append(to_string(total_ordering_stage));
    information_message.append("$");
    information_message.append(UUID);

    return information_message;
}

/**
 * @brief The function responsible for delivering messages to their assoiated group.
 *
 * @param listen_fd The sock fd from which we're sending from (the current server/what it read)
 * @param group The group the message originated from / should be sent to
 * @param buffer The buffer that we're sending along to be processes
 * @param buffer_len The length of the buffer for sendto()
 */
void deliver_to_group(int listen_fd, int group, char buffer[], int buffer_len) {
    for (client_t cur_client : all_clients) {
        if (cur_client.group == group) {
            sendto(listen_fd, buffer, buffer_len, 0, (struct sockaddr *)&cur_client.address,
                   cur_client.address_size);
        }
    }
}

/**
 * @brief The function responsible for forwarded messages received on this server to other servers
 *
 * @param listen_fd The socket we received the message on to begin with
 * @param buffer The message to send along
 * @param buffer_len The length of the received message for sending purposes
 */
void forward_message(int listen_fd, char buffer[], int buffer_len) {
    // Send buffer to every server except ourself so they can process appropriately
    for (sockaddr_in server : all_servers) {
        if (server.sin_addr.s_addr != SELF_SERVER.sin_addr.s_addr &&
            server.sin_port != SELF_SERVER.sin_port) {
            sendto(listen_fd, buffer, buffer_len, 0, (struct sockaddr *)&server, sizeof(server));
        }
    }
}

/**
 * @brief Checks if the sockaddr_in for the current message to the server hppens to be a current
 * Server or not.
 *
 * @param src The sockaddr_in of the messaging connection
 */
bool src_is_server(sockaddr_in src) {
    for (sockaddr_in cur_server : all_servers) {
        if (cur_server.sin_addr.s_addr == src.sin_addr.s_addr &&
            cur_server.sin_port == src.sin_port) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Checks if the sockaddr_in for the current message to the server is the same as itself.
 * This is possible in the case of total ordering where we send requests to every server and then
 * bring them all back to find the proposal.
 *
 * @param src - The sockaddr_in of the messaging connection
 * @return true - True if this src is the same as ourself
 * @return false - Otherwise
 */
bool src_is_self(sockaddr_in src) {
    if (SELF_SENDING_ADDR.sin_addr.s_addr == src.sin_addr.s_addr &&
        SELF_SENDING_ADDR.sin_port == src.sin_port) {
        return true;
    }

    return false;
}

/**
 * @brief Checks if the sockaddr_in for the current message to the server happens to be a current
 * Client or not.
 *
 * @param src The sockaddr_in of the messaging connection
 */
bool src_is_client(sockaddr_in src) {
    for (client_t cur_client : all_clients) {
        if (cur_client.address.sin_addr.s_addr == src.sin_addr.s_addr &&
            cur_client.address.sin_port == src.sin_port) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Gets the client object matching the src that sent the message
 *
 * @param src The sockaddr_in the message originated from
 * @return client_t* A pointer to the client object
 */
client_t *get_client(sockaddr_in src) {
    for (int i = 0; i < all_clients.size(); i++) {
        client_t cur_client = all_clients[i];
        if (cur_client.address.sin_addr.s_addr == src.sin_addr.s_addr &&
            cur_client.address.sin_port == src.sin_port) {
            return &all_clients[i];
        }
    }
    return NULL;
}

/**
 * @brief Depending on the type of ordering the server was initialized with deliver messages
 * appropriately. Unordered ordering simply sends all the message to all clients,
 * FIFO ordering follows the algorithm given in the slides, and total ordering follows
 * the distributed procedure described in lecture, depending on the stage of the procedure.
 *
 * @param sender - The other server node which the message originated rom
 * @param group - The group the message should be distributed to
 * @param msg_id - The message id / sequence number per slides for fifo/total ordering
 * @param buffer - The buffer of the actual message to be sent out
 * @param buffer_len - The length of the message buffer
 * @param total_ordering_stage - The stage of total ordering we're in. Includes COLLECT_PROPOSALS
 * (Each node gives its number) PROCESS_PROPOSALS (We check the proposal from each node and chose
 * the max), and DELIVER_MESSAGES (All proposals have been updated so distribute the message)
 * @param uuid - The string UUID of the message so we can uniquly identify it during Total ordering
 */
void ordered_delivery(int sender, int group, int msg_id, char buffer[], int buffer_len,
                      int total_ordering_stage, string uuid) {

    if (ORDERING == UNORDERED_ORDERING) {
        // Deliver to each client in the group
        log_message("Received unordered distribution request.");
        deliver_to_group(SELF_SOCKET, group, buffer, buffer_len);
    }

    if (ORDERING == FIFO_ORDERING) {
        log_message("Received FIFO distribution request. Populating HOLDBACK and delivering.");
        // Follow lecture procedure for B-Deliver for FIFO
        F_HOLDBACK[sender][group][msg_id] = string(buffer);

        int next_id = R[sender][group] + 1;
        while (F_HOLDBACK[sender][group].find(next_id) != F_HOLDBACK[sender][group].end()) {
            // FO-Deliver
            string message = F_HOLDBACK[sender][group][next_id];
            deliver_to_group(SELF_SOCKET, group, (char *)message.c_str(), message.size());
            F_HOLDBACK[sender][group].erase(next_id);
            R[sender][group]++;
            next_id = R[sender][group] + 1;
        }
    }

    if (ORDERING == TOTAL_ORDERING) {
        log_message(
            "Received total ordering message request. Group: %d Sequence: %d Node: %d Stage : %s",
            group, msg_id, sender,
            total_ordering_stage == COLLECT_PROPOSALS   ? "Collecting Proposals / Sending Back"
            : total_ordering_stage == PROCESS_PROPOSALS ? "Processing Proposals"
                                                        : "Delivering Proposals");

        message_t cur_message;
        cur_message.sequence_number = msg_id;
        cur_message.sender_id = sender;
        cur_message.is_deliverable = false;
        cur_message.UUID = uuid;
        cur_message.message = string(buffer);

        // Step 1 of the slides where every server sends its proposal
        if (total_ordering_stage == COLLECT_PROPOSALS) {
            log_message("Creating proposal and sending back to original sender");
            // Each recipient i responds with its proposed number updating
            P[group] = max(P[group], A[group]) + 1;
            cur_message.sequence_number = P[group];

            // Then puts (m, P_new, i) into its local hold-back queue for group g, but marks m
            // unreadable
            T_HOLDBACK[group][cur_message] = uuid;

            string proposal_respond_string =
                create_forward_message(group, buffer, P[group], PROCESS_PROPOSALS, uuid);

            if (sender == CONFIGURATION_INDEX) {
                // If we're on the server that originated the
                // collect process send to ourself
                sendto(SELF_SOCKET, (char *)proposal_respond_string.c_str(),
                       proposal_respond_string.size(), 0, (struct sockaddr *)&SELF_SERVER,
                       sizeof(SELF_SERVER));
            } else {
                // Otherwise use the all_inc_cur list to send back ()
                // Note unlike all_servers, all_inc_cur includes the current server
                // As otherwise we can't compute the correct list index to send to
                int calc_index = sender - 1;
                sendto(SELF_SOCKET, (char *)proposal_respond_string.c_str(),
                       proposal_respond_string.size(), 0,
                       (struct sockaddr *)&all_inc_cur[abs(calc_index)],
                       sizeof(all_inc_cur[abs(calc_index)]));
            }
        }

        if (total_ordering_stage == PROCESS_PROPOSALS) {
            log_message("Total ordering entered the process proposal stage.");
            log_message("Collecting proposal.");
            T_PROPOSALS[uuid].push_back(cur_message);

            if (T_PROPOSALS[uuid].size() == all_servers.size() + 1) {
                log_message("Collected all necessary proposals. Finding maximum.");

                message_t cur_max_proposal;
                cur_max_proposal.sequence_number = -1;
                cur_max_proposal.sender_id = -1;

                for (int i = 0; i < T_PROPOSALS[uuid].size(); i++) {
                    if (T_PROPOSALS[uuid][i] > cur_max_proposal) {
                        cur_max_proposal = T_PROPOSALS[uuid][i];
                    }
                }

                log_message("Maximum proposal found - sending back for delivery stage.");

                string process_respond_string = create_forward_message(
                    group, buffer, cur_max_proposal.sequence_number, DELIVER_MESSAGES, uuid);

                forward_message(SELF_SOCKET, (char *)process_respond_string.c_str(),
                                process_respond_string.size());
                sendto(SELF_SOCKET, (char *)process_respond_string.c_str(),
                       process_respond_string.size(), 0, (struct sockaddr *)&SELF_SERVER,
                       sizeof(SELF_SERVER));
            }
        }

        if (total_ordering_stage == DELIVER_MESSAGES) {
            log_message("Delivering messages in total order.");

            // Set the message to deliverable
            cur_message.is_deliverable = true;

            // Find the message to remove - this is the first message equal since the map
            // Sorts in order of sequence/server id for us
            message_t msg_to_remove;
            for (auto &it : T_HOLDBACK[group]) {
                if (it.first == cur_message) {
                    msg_to_remove = it.first;
                    break;
                }
            }

            // Update the message to the highest sequence / sender id. Can't modify in place so
            // remove & replace
            T_HOLDBACK[group].erase(msg_to_remove);
            T_HOLDBACK[group][cur_message] = cur_message.UUID;

            // Update agreed
            A[group] = max(A[group], cur_message.sequence_number);

            // Deliver deliverable messages in order, keep track of those that are delivered so
            // we can remove them from the holdback queue
            vector<message_t> remove_messages;
            for (auto &it : T_HOLDBACK[group]) {
                if (it.first.is_deliverable) {
                    deliver_to_group(SELF_SOCKET, group, (char *)it.first.message.c_str(),
                                     it.first.message.size());
                    remove_messages.push_back(it.first);
                }
            }

            for (message_t cur_remove : remove_messages) {
                T_HOLDBACK[group].erase(cur_remove);
            }
        }
    }
}

/**
 * @brief This function handles all client commands. When a command is sent from a client
 * We detect if its a command or a message, and act accordingly
 *
 * @param calling_client The client_t object that originated the call, so we can use their current
 * info
 * @param buffer The buffer which was read in to the server
 * @param buffer_len The length of the buffer we just read in
 */
void client_handler(client_t &calling_client, char buffer[], int buffer_len) {
    // Sanity check that we have a buffer - not sure if this is necessary but just in case
    if (buffer_len < 1) {
        return;
    }

    // Detect if this was a command or a message
    if (buffer[0] == '/') {
        log_message("Client sent a command.");
        vector<string> params;
        split_tokens_given_token(params, buffer, buffer_len, " ");

        if (str_equals_given_str(params[0], "/join")) {
            if (params.size() < 2) {
                string invalid_join = "-ERR Invalid usage of join command.";
                sendto(SELF_SOCKET, (char *)invalid_join.c_str(), invalid_join.size(), 0,
                       (struct sockaddr *)&calling_client.address, calling_client.address_size);
                log_message("Invalid usage of join command.");
                return;
            }

            if (calling_client.group != INVALID_GROUP) {
                int cur_group = calling_client.group;
                string in_group_error = "-ERR You are already in chat room #";
                in_group_error.append(to_string(cur_group));
                sendto(SELF_SOCKET, (char *)in_group_error.c_str(), in_group_error.size(), 0,
                       (struct sockaddr *)&calling_client.address, calling_client.address_size);
                log_message("Client already in another chat room.");
                return;
            }

            // Grab the number the person is trying to join
            // Note to self: use atoi instead of stoi b.c atoi wont throw an error on malformed
            // input, It will just set the number to 0
            int new_group = atoi(params[1].c_str());

            // Check that the new group is in valid range, then set the group accordingly
            if (new_group <= 0 || new_group > MAX_NUM_GROUPS) {
                string out_of_group_range =
                    "-ERR Entered group is out of permissible range (1-50).";
                sendto(SELF_SOCKET, (char *)out_of_group_range.c_str(), out_of_group_range.size(),
                       0, (struct sockaddr *)&calling_client.address, calling_client.address_size);
                log_message("Entered room [%d] is out of range, cannot join ", new_group);
                return;
            }

            calling_client.group = new_group;
            string successful_join = "+OK You are now in chat room #";
            successful_join.append(to_string(new_group));
            sendto(SELF_SOCKET, (char *)successful_join.c_str(), successful_join.size(), 0,
                   (struct sockaddr *)&calling_client.address, calling_client.address_size);
            log_message("Client successfully joined room [%d]", new_group);
        } else if (str_equals_given_str(params[0], "/part")) {
            // If the user isn't currently in a group send an error message
            if (calling_client.group == INVALID_GROUP) {
                string invalid_part = "-ERR Not currently in a group to leave.";
                sendto(SELF_SOCKET, (char *)invalid_part.c_str(), invalid_part.size(), 0,
                       (struct sockaddr *)&calling_client.address, calling_client.address_size);
                log_message("Client entered part while not currently being in a room.");
                return;
            }

            // Since the user is currently in a group set it to invalid and send suceess message.
            int previous_group = calling_client.group;
            calling_client.group = INVALID_GROUP;
            string successful_part = "+OK You have left chat room #";
            successful_part.append(to_string(previous_group));
            sendto(SELF_SOCKET, (char *)successful_part.c_str(), successful_part.size(), 0,
                   (struct sockaddr *)&calling_client.address, calling_client.address_size);
            log_message("Client succesfully left group [%d]", previous_group);
        } else if (str_equals_given_str(params[0], "/nick")) {
            if (params.size() < 2) {
                string invalid_nick = "-ERR Invalid usage of nick command.";
                sendto(SELF_SOCKET, (char *)invalid_nick.c_str(), invalid_nick.size(), 0,
                       (struct sockaddr *)&calling_client.address, calling_client.address_size);
                log_message("Invalid use of nickname command.");
                return;
            }

            // Set the client nickname to the entered parameter
            calling_client.nick = string(params[1]);

            // If the nickname has spaces we append all words following it
            if (params.size() > 2) {
                for (int i = 2; i < params.size(); i++) {
                    calling_client.nick.append(" ");
                    calling_client.nick.append(params[i]);
                }
            }

            string successful_nick = "+OK Your nickname is now: ";
            successful_nick.append(calling_client.nick);
            sendto(SELF_SOCKET, (char *)successful_nick.c_str(), successful_nick.size(), 0,
                   (struct sockaddr *)&calling_client.address, calling_client.address_size);
            log_message("Client nickname set to %s", (char *)calling_client.nick.c_str());
        } else if (str_equals_given_str(params[0], "/quit")) {
            log_message("Client entered quit command.");
            // Just to be safe we do the same /part procedure and set it to an invalid group
            calling_client.group = INVALID_GROUP;

            // Remove the client from our list of current clients
            int index = 0;
            for (int i = 0; i < all_clients.size(); i++) {
                if (all_clients[i] == calling_client) {
                    index = i;
                }
            }

            log_message("Removing client at index [%d] from current clients.", index);

            all_clients.erase(all_clients.begin() + index);
        } else {
            string invalid_cmd = "-ERR Invalid Command.";
            sendto(SELF_SOCKET, (char *)invalid_cmd.c_str(), invalid_cmd.size(), 0,
                   (struct sockaddr *)&calling_client.address, calling_client.address_size);
            log_message("Client entered invalid command.");
        }
    } else {
        if (calling_client.group == INVALID_GROUP) {
            string not_in_group = "-ERR Cannot send message - not in a chatroom.";
            sendto(SELF_SOCKET, (char *)not_in_group.c_str(), not_in_group.size(), 0,
                   (struct sockaddr *)&calling_client.address, calling_client.address_size);
            log_message("Client sent message while not being in a group.");
            return;
        }

        // This is a chat message so append nick/sender info and send for real delivery
        string full_message = "<";
        full_message.append(calling_client.nick);
        full_message.append("> ");
        full_message.append(buffer);

        // If we're not doing total ordering send this to the originating server's clients instantly
        if (ORDERING != TOTAL_ORDERING) {
            log_message("Delivering client message.");
            deliver_to_group(SELF_SOCKET, calling_client.group, (char *)full_message.c_str(),
                             full_message.size());
        }

        // Create the current message UUID using the current time and sender info
        // message uuid is in the form: SEVER ID/TIME IN SECODNS /MESSAGE COUNT
        struct timeval tv;
        gettimeofday(&tv, NULL);
        string cur_message_uuid = to_string(CONFIGURATION_INDEX) + "/" + to_string(tv.tv_sec) +
                                  "/" + to_string(++message_count);

        // Create a message holding all necessary forwarding info and send to the other servers
        string forward_info_str = create_forward_message(
            calling_client.group, (char *)full_message.c_str(),
            ORDERING == FIFO_ORDERING ? ++S[calling_client.group] : 0,
            ORDERING == TOTAL_ORDERING ? COLLECT_PROPOSALS : NOT_TOTAL_ORDER, cur_message_uuid);
        forward_message(SELF_SOCKET, (char *)forward_info_str.c_str(), forward_info_str.size());

        // If we're doing total ordering we also need to get a message from ourself per slides for
        // proposals, so we send proposal request to ourself
        if (ORDERING == TOTAL_ORDERING) {
            log_message("Delivering client message using total ordering.");
            sendto(SELF_SOCKET, (char *)forward_info_str.c_str(), forward_info_str.size(), 0,
                   (struct sockaddr *)&SELF_SERVER, sizeof(SELF_SERVER));
        }
    }
}

/**
 * @brief The main function the server is run from - this handles reading all information
 * sent to the server and acting accordingly. If messages originate from servers we follow
 * the delivery ordering specified on loadup and send all necessary messages out correctly,
 * otherwise if mesages originate from clients we either execute the specified command
 * or begin distributing the chat message.
 */
void run_server() {
    // This should hold the main loop where we receive UDP messages
    // Should I rename listen_fd to sock since this isnt stream?
    int listen_fd = socket(PF_INET, SOCK_DGRAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "Cannot open socket (%s)\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Initialize global SELF_SOCKET to the socket for when we send messages in other places
    SELF_SOCKET = listen_fd;

    int opt = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)) < 0) {
        fprintf(stderr, "Cannot set socket options (%s)\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    SELF_SERVER.sin_addr.s_addr = htons(INADDR_ANY);

    int bind_ret = bind(listen_fd, (struct sockaddr *)&SELF_SERVER, sizeof(SELF_SERVER));
    if (bind_ret < 0) {
        fprintf(stderr, "Cannot bind socket (%s)\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    log_message("Server [%d] initialized - now accepting messages.", CONFIGURATION_INDEX);

    while (true) {
        // Getting the source address to receive messages from
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);

        // The buffer we read to along with our recvfrom call - note we might want to read
        // sizeof(buffer) - 1 per the slides, but we haven't before so leaving it.
        char buffer[MAX_LINE_LENGTH] = {0};
        int rlen =
            recvfrom(listen_fd, buffer, sizeof(buffer), 0, (struct sockaddr *)&src, &src_len);

        // If for some reason we don't read anything just repeat
        if (rlen <= 0) {
            continue;
        }

        if (src_is_server(src) || src_is_self(src)) {

            // Information message format: sender$msg_id/seq_num$group$message
            log_message("Received message '%s' from another server.", buffer);

            // Tokenize all the fields sent in the info message for usage
            vector<string> message_params;
            split_tokens_given_token(message_params, buffer, rlen, "$");

            // Deliver received message to clients following ordering scheme
            ordered_delivery(atoi(message_params[0].c_str()), atoi(message_params[2].c_str()),
                             atoi(message_params[1].c_str()), (char *)message_params[3].c_str(),
                             message_params[3].size(), atoi(message_params[4].c_str()),
                             message_params[5]);
        } else {
            if (!src_is_client(src)) {
                client_t newest_client;
                newest_client.address = src;
                newest_client.address_size = src_len;
                newest_client.group = INVALID_GROUP;
                newest_client.nick = string(inet_ntoa(src.sin_addr));
                newest_client.nick.append(":");
                newest_client.nick.append(to_string(ntohs(src.sin_port)));
                all_clients.push_back(newest_client);
                log_message("Registered new client");
            }

            log_message("Received message '%s' from a client.", buffer);

            // Get the calling client for our handler to work with
            client_t *calling_client = get_client(src);
            client_handler(*calling_client, buffer, rlen);
        }
    }
}

/**
 * @brief Initializes all servers from the configuration file, making
 * SELF_SERVER equal to the server at the correct configuration index
 *
 * @param a The name of the configuration file
 * @param b The index of the current server in the configuration file
 */
void initialize_servers(string CONFIGURATION_FILE) {
    // Open the CONFIGURATION_FILE
    ifstream config_file(CONFIGURATION_FILE);

    // If file doesn't exist exit - behavior not specified in writeup, so we just to exit and print
    // an error
    if (!config_file) {
        log_message("Configuration file does not exist - cannot open.");
        exit(EXIT_FAILURE);
    }

    log_message("Initializing servers from specified configuration file.");
    int index = 0;
    string line;
    while (getline(config_file, line)) {
        // Parse the read line seperated by a comma - if there is 1 elem we're not using a proxy
        // If there are two elems and we're at the index of ourself we use the proxy
        vector<string> parsed_line, addresses_info;
        split_tokens_given_token(parsed_line, (char *)line.c_str(), line.size(), ",");

        if (++index == CONFIGURATION_INDEX) {
            // If we're using a proxy there'll be two elems, grab the second to bind
            // Otherwise just use the first.
            string addr_info = parsed_line.size() == 2 ? parsed_line[1] : parsed_line[0];
            split_tokens_given_token(addresses_info, (char *)addr_info.c_str(), addr_info.size(),
                                     ":");

            log_message("Configuring this server with ip: %s and port: %s",
                        (char *)addresses_info[0].c_str(), (char *)addresses_info[1].c_str());

            // Configure self server, then skip to next iteration
            bzero(&SELF_SERVER, sizeof(SELF_SERVER));
            SELF_SERVER.sin_family = AF_INET;
            SELF_SERVER.sin_port = htons(stoi(addresses_info[1]));
            inet_pton(AF_INET, addresses_info[0].c_str(), &SELF_SERVER.sin_addr);
            all_inc_cur.push_back(SELF_SERVER);

            // Configure self sending server - where we send from. We need to save this info before
            // We bind in run_server otherwise we can't detect when we've sent ourself a message
            // Due to the fact we change our sin.addr when binding
            bzero(&SELF_SENDING_ADDR, sizeof(SELF_SENDING_ADDR));
            SELF_SENDING_ADDR.sin_family = AF_INET;
            SELF_SENDING_ADDR.sin_port = htons(stoi(addresses_info[1]));
            inet_pton(AF_INET, addresses_info[0].c_str(), &SELF_SENDING_ADDR.sin_addr);
            continue;
        }

        // Split the current line given we're not at the current index. Note this is always the
        // first index since we don't store other servers proxy bind addresses
        split_tokens_given_token(addresses_info, (char *)parsed_line[0].c_str(),
                                 parsed_line[0].size(), ":");

        log_message("Configuring node with ip: %s and port: %s", (char *)addresses_info[0].c_str(),
                    (char *)addresses_info[1].c_str());
        // The server we're adding, calculated based off the first index (since its not the cur
        // server)
        struct sockaddr_in dest;
        bzero(&dest, sizeof(dest));
        dest.sin_family = AF_INET;
        dest.sin_port = htons(stoi(addresses_info[1]));
        inet_pton(AF_INET, addresses_info[0].c_str(), &dest.sin_addr);

        // Add the server to both the all_servers list which is used for sending messages, and
        // all_inc_cur which includes the current for Total ordering
        all_servers.push_back(dest);
        all_inc_cur.push_back(dest);
    }
}

/**
 * @brief The main function of the program. Here we initialize all the global information
 * needed for the current server and initialize all other information from the configuration file.
 * Finally a call to run_server runs the main loop of the server.
 *
 * @param argc
 * @param argv
 * @return int
 */
int main(int argc, char *argv[]) {

    // If no arguments are provided print name and PennKey
    if (argc < 2) {
        fprintf(stderr, "*** Author: Leon Hertzberg (leonjh)\n");
        exit(EXIT_SUCCESS);
    }

    int opt;
    while ((opt = getopt(argc, argv, "vo:")) != -1) {
        switch (opt) {
        case 'v':
            VERBOSE = true;
            break;
        case 'o':
            if (strcmp(optarg, "unordered") == 0) {
                ORDERING = UNORDERED_ORDERING;
            } else if (strcmp(optarg, "total") == 0) {
                ORDERING = TOTAL_ORDERING;
            } else if (strcmp(optarg, "fifo") == 0) {
                ORDERING = FIFO_ORDERING;
            } else {
                fprintf(stderr, "Invalid and/or no type of ordering entered.\nOptions "
                                "are 'unordered', 'total', or 'fifo'.\n");
                exit(EXIT_FAILURE);
            }
            break;
        case '?':
            printf("Use correct format: ./echoserver -o (ordering type) "
                   "(optional: -v) <configuration_file> <index>\n");
            exit(EXIT_SUCCESS);
        default:
            /* Add printing full name / login when no args are given*/
            fprintf(stderr, "Leon Hertzberg - leonjh");
            exit(EXIT_SUCCESS);
        }
    }

    // Check that we have two more arguments as we still require the configuration file as well as
    // the configuration index
    if (argc - optind < 2) {
        printf("Use correct format: ./echoserver -o (ordering type) "
               "(optional: -v) <configuration_file> <index>\n");
        exit(EXIT_FAILURE);
    }

    // Set the CONFIGURATING_FILE and CONFIGURATION_INDEX (Current server ID) as provided
    string CONFIGURATION_FILE(argv[optind++]);
    CONFIGURATION_INDEX = atoi(argv[optind]);
    log_message("Configuration file set to %s", (char *)CONFIGURATION_FILE.c_str());
    log_message("Configuration index set to %d", CONFIGURATION_INDEX);
    log_message("Server ordering set to %s", ORDERING == UNORDERED_ORDERING ? "Unordered"
                                             : ORDERING == FIFO_ORDERING    ? "FIFO Ordering"
                                                                            : "Total Ordering");

    // Initialize all the servers from the configuration file
    initialize_servers(CONFIGURATION_FILE);

    // Run the main loop for the server which handles all functionality
    run_server();

    return 0;
}
