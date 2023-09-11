#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <cerrno>
#include <iostream>
#include <string>
#include <vector>

using namespace std;

using namespace std;

/**
 * @brief Repairs the string after its been tokenized
 *
 * @param cmd - the char pointer array
 * @param cmd_length - the length of the string
 */
void repair_string(char cmd[], int cmd_length) {
    for (int i = 0; i < cmd_length; i++) {
        if (cmd[i] == '\0') {
            cmd[i] = ' ';
        }
    }
}

/**
 * Counts the number of tokens in a string and returns the count.
 * Also repairs the string so that it may be used again
 * Takes in the string of which tokens it is counting, and the length of the
 * strung
 */
int count_tokens(char cmd[], int cmd_length) {
    int count = 0;
    fprintf(stderr, "Here %s", cmd);
    ;
    char *token;
    char *rest = &cmd[0];
    while ((token = strtok_r(rest, " ", &rest))) {
        printf("Token: %s\n", token);
        count++;
    }

    repair_string(cmd, cmd_length);

    return count;
}

/**
 * @brief Populates a vector with the tokens in a string
 *
 * @param strings the vector to store the strings
 * @param cmd the string to tokenize
 */
void split_tokens(vector<string> &strings, char cmd[], int cmd_length) {
    char *token;
    char *rest = &cmd[0];
    while ((token = strtok_r(rest, " ", &rest))) {
        string token_string(token);
        strings.push_back(token_string);
    }

    repair_string(cmd, cmd_length);
}

void split_tokens_no_crlf(vector<string> &strings, char cmd[], int cmd_length) {
    char *token;
    char *rest = &cmd[0];
    while ((token = strtok_r(rest, " ", &rest))) {
        for (int i = 0; i < strlen(token); i++) {
            if (token[i] == '\r' || token[i] == '\n') {
                token[i] = 0;
            }
        }

        strings.push_back(string(token));
    }

    repair_string(cmd, cmd_length);
}

// void split_tokens_given_token(vector<string> &strings, char cmd[],
//                               int cmd_length, string pattern)
// {
//     char *token;
//     char *rest = &cmd[0];
//     while ((token = strtok_r(rest, pattern.c_str(), &rest)))
//     {
//         for (int i = 0; i < strlen(token); i++)
//         {
//             if (token[i] == '\r' || token[i] == '\n')
//             {
//                 token[i] = 0;
//             }
//         }
//
//         strings.push_back(string(token));
//     }
//
//     repair_string(cmd, cmd_length);
// }

void split_tokens_given_token(vector<string> &strings, char cmd[],
                              int cmd_length, const char *pattern) {
    char *token;
    char *rest = &cmd[0];
    while ((token = strtok_r(rest, pattern, &rest))) {
        for (int i = 0; i < strlen(token); i++) {
            if (token[i] == '\r' || token[i] == '\n') {
                token[i] = 0;
            }
        }

        strings.push_back(string(token));
    }

    repair_string(cmd, cmd_length);
}
