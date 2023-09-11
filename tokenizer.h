#pragma once
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

int count_tokens(char cmd[], int cmd_length);

void split_tokens(vector<string> &strings, char cmd[], int cmd_length);

void repair_string(char cmd[], int cmd_length);

void split_tokens_no_crlf(vector<string> &strings, char cmd[], int cmd_length);

// void split_tokens_given_token(vector<string> &strings, char cmd[],
// int cmd_length, string pattern);

void split_tokens_given_token(vector<string> &strings, char cmd[],
                              int cmd_length, const char *pattern);
