#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdbool.h>

#define MAX_CLIENTS 100
#define BUFFER_SIZE 4096
#define NICK_LEN 32
#define ROOM_LEN 32
#define TOPIC_LEN 256 // Max length for a room topic
#define MAX_ROOMS 50  // Max number of rooms with custom topics we can store

const char *MOTD = "=== Welcome to the Simple C Chat Server! ===\n"
                   "Rules: Be respectful.\n"
                   "==========================================\n";

// --- Room Topic Management ---
typedef struct
{
    char name[ROOM_LEN];
    char topic[TOPIC_LEN];
} room_info_t;

room_info_t room_topics[MAX_ROOMS];
pthread_mutex_t room_topics_mutex = PTHREAD_MUTEX_INITIALIZER;
const char *DEFAULT_TOPIC = "No topic set.";

typedef struct
{
    int sockfd; // This stores the unique ID for the client's connection
    struct sockaddr_in address;
    char nickname[NICK_LEN];
    char room[ROOM_LEN];
    pthread_t tid;
} client_t;

client_t *clients[MAX_CLIENTS];
int client_count = 0;
pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;

const char *DEFAULT_ROOM = "Lobby";

void add_client(client_t *cl)
{
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i)
    {
        if (!clients[i])
        {
            clients[i] = cl;
            client_count++;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}
void remove_client(int sockfd)
{
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i)
    {
        if (clients[i] && clients[i]->sockfd == sockfd)
        {
            clients[i] = NULL;
            client_count--;
            break;
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}
void send_to_client(int sockfd, const char *message)
{
    if (write(sockfd, message, strlen(message)) < 0)
    {
    } // Ignore write errors silently for now
}
void broadcast_room(const char *message, int sender_sockfd, const char *room_name)
{
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i)
    {
        if (clients[i] && clients[i]->sockfd != sender_sockfd && strcmp(clients[i]->room, room_name) == 0)
        {
            send_to_client(clients[i]->sockfd, message);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}
void broadcast_system_room(const char *message, const char *room_name)
{
    pthread_mutex_lock(&clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; ++i)
    {
        if (clients[i] && strcmp(clients[i]->room, room_name) == 0)
        {
            send_to_client(clients[i]->sockfd, message);
        }
    }
    pthread_mutex_unlock(&clients_mutex);
}

int find_or_create_room_info_index(const char *room_name)
{
    int free_slot = -1;
    for (int i = 0; i < MAX_ROOMS; ++i)
    {
        // If name is empty, it's a potential free slot
        if (strlen(room_topics[i].name) == 0 && free_slot == -1)
        {
            free_slot = i;
        }
        // If room found, return its index
        if (strcmp(room_topics[i].name, room_name) == 0)
        {
            return i;
        }
    }
    // Room not found, try to use the free slot
    if (free_slot != -1)
    {
        strncpy(room_topics[free_slot].name, room_name, ROOM_LEN - 1);
        room_topics[free_slot].name[ROOM_LEN - 1] = '\0';
        strncpy(room_topics[free_slot].topic, DEFAULT_TOPIC, TOPIC_LEN - 1);
        room_topics[free_slot].topic[TOPIC_LEN - 1] = '\0';
        return free_slot;
    }
    // No existing room and no free slots
    return -1;
}

void *handle_client(void *arg)
{
    char buffer[BUFFER_SIZE];
    char message_buffer[BUFFER_SIZE + NICK_LEN + ROOM_LEN + TOPIC_LEN + 50];
    int leave_flag = 0;
    client_t *cli = (client_t *)arg;

    //  Send MOTD first
    send_to_client(cli->sockfd, MOTD);

    printf("INFO: Client connected: %s:%d (assigned nick: %s, room: %s)\n",
    inet_ntoa(cli->address.sin_addr), ntohs(cli->address.sin_port), cli->nickname, cli->room);

    sprintf(message_buffer, "SYSTEM: Welcome! Your nick is %s. You are in room '%s'.\n", cli->nickname, cli->room);
    send_to_client(cli->sockfd, message_buffer);
    // * NEW: Updated help message *
    sprintf(message_buffer, "SYSTEM: Use /nick <nick>, /join <room>, /topic [new_topic], /users, /list, /quit.\n");
    send_to_client(cli->sockfd, message_buffer);

    // * NEW: Send initial room topic *
    pthread_mutex_lock(&room_topics_mutex);
    int topic_idx = find_or_create_room_info_index(cli->room);
    if (topic_idx != -1)
    {
        sprintf(message_buffer, "SYSTEM: Topic for '%s' is: %s\n", cli->room, room_topics[topic_idx].topic);
    }
    else
    {
        sprintf(message_buffer, "SYSTEM: Topic for '%s': %s\n", cli->room, DEFAULT_TOPIC); // Fallback
    }
    pthread_mutex_unlock(&room_topics_mutex);
    send_to_client(cli->sockfd, message_buffer);

    sprintf(message_buffer, "SYSTEM: User '%s' has joined room '%s'.\n", cli->nickname, cli->room);
    broadcast_system_room(message_buffer, cli->room);

    // Główna pętla odbierania wiadomości od klienta
    while (leave_flag == 0)
    {
        memset(buffer, 0, BUFFER_SIZE);
        // Receive message from client
        int receive = recv(cli->sockfd, buffer, BUFFER_SIZE - 1, 0);


        if (receive > 0)
        {
            buffer[receive] = '\0';
            buffer[strcspn(buffer, "\r\n")] = 0;
            if (strlen(buffer) == 0)
                continue;

            if (buffer[0] == '/')
            {
                char command[BUFFER_SIZE];
                char argument[BUFFER_SIZE] = {0}; // Initialize argument buffer
                char *space_ptr = strchr(buffer, ' ');
                bool has_argument = (space_ptr != NULL);

                // Extract command part
                size_t cmd_len = has_argument ? (space_ptr - buffer) : strlen(buffer);
                if (cmd_len >= BUFFER_SIZE)
                    cmd_len = BUFFER_SIZE - 1;
                strncpy(command, buffer, cmd_len);
                command[cmd_len] = '\0';

                // Extract argument part if exists
                if (has_argument)
                {
                    // Skip leading spaces in argument if any
                    char *arg_start = space_ptr + 1;
                    while (*arg_start == ' ' && *arg_start != '\0')
                    {
                        arg_start++;
                    }
                    size_t arg_len = strlen(arg_start);
                    if (arg_len >= BUFFER_SIZE)
                        arg_len = BUFFER_SIZE - 1;
                    strncpy(argument, arg_start, arg_len);
                    argument[arg_len] = '\0';
                }

                // --- Command Handling ---
                if (strcmp(command, "/users") == 0 || strcmp(command, "/who") == 0)
                {
                    // [/users] code as before...
                    char user_list_buffer[BUFFER_SIZE] = {0};
                    bool first_user = true;
                    int current_len = 0;
                    pthread_mutex_lock(&clients_mutex);
                    current_len += snprintf(user_list_buffer + current_len, BUFFER_SIZE - current_len, "SYSTEM: Users in room '%s': [", cli->room);
                    for (int i = 0; i < MAX_CLIENTS; ++i)
                    {
                        if (clients[i] && strcmp(clients[i]->room, cli->room) == 0)
                        {
                            if (!first_user)
                                current_len += snprintf(user_list_buffer + current_len, BUFFER_SIZE - current_len, ", ");
                            current_len += snprintf(user_list_buffer + current_len, BUFFER_SIZE - current_len, "%s", clients[i]->nickname);
                            if (current_len >= BUFFER_SIZE - (NICK_LEN + 5))
                            {
                                snprintf(user_list_buffer + current_len, BUFFER_SIZE - current_len, "...");
                                break;
                            }
                            first_user = false;
                        }
                    }
                    if (current_len < BUFFER_SIZE - 3)
                        strcat(user_list_buffer, "]\n");
                    else
                        strcpy(user_list_buffer + BUFFER_SIZE - 4, "]\n");
                    pthread_mutex_unlock(&clients_mutex);
                    send_to_client(cli->sockfd, user_list_buffer);
                }
                else if (strcmp(command, "/list") == 0)
                {
                    // [/list] code as before...
                    char room_list_buffer[BUFFER_SIZE] = {0};
                    char unique_rooms[MAX_CLIENTS][ROOM_LEN];
                    int unique_room_count = 0;
                    bool first_room = true;
                    int current_len = 0;
                    pthread_mutex_lock(&clients_mutex);
                    for (int i = 0; i < MAX_CLIENTS; ++i)
                    {
                        if (clients[i])
                        {
                            bool found = false;
                            for (int j = 0; j < unique_room_count; ++j)
                                if (strcmp(clients[i]->room, unique_rooms[j]) == 0)
                                {
                                    found = true;
                                    break;
                                }
                            if (!found && unique_room_count < MAX_CLIENTS)
                            {
                                strncpy(unique_rooms[unique_room_count], clients[i]->room, ROOM_LEN - 1);
                                unique_rooms[unique_room_count][ROOM_LEN - 1] = '\0';
                                unique_room_count++;
                            }
                        }
                    }
                    current_len += snprintf(room_list_buffer + current_len, BUFFER_SIZE - current_len, "SYSTEM: Available rooms: ");
                    for (int j = 0; j < unique_room_count; ++j)
                    { /* ... count users and build string ... */
                        int users_in_room = 0;
                        for (int i = 0; i < MAX_CLIENTS; ++i)
                            if (clients[i] && strcmp(clients[i]->room, unique_rooms[j]) == 0)
                                users_in_room++;
                        if (!first_room)
                            current_len += snprintf(room_list_buffer + current_len, BUFFER_SIZE - current_len, ", ");
                        current_len += snprintf(room_list_buffer + current_len, BUFFER_SIZE - current_len, "%s (%d)", unique_rooms[j], users_in_room);
                        if (current_len >= BUFFER_SIZE - (ROOM_LEN + 10))
                        {
                            snprintf(room_list_buffer + current_len, BUFFER_SIZE - current_len, "...");
                            break;
                        }
                        first_room = false;
                    }
                    pthread_mutex_unlock(&clients_mutex);
                    if (unique_room_count == 0)
                        strcat(room_list_buffer, "None");
                    if (current_len < BUFFER_SIZE - 2)
                        strcat(room_list_buffer, "\n");
                    else
                        strcpy(room_list_buffer + BUFFER_SIZE - 3, "\n");
                    send_to_client(cli->sockfd, room_list_buffer);
                }
                // * NEW: /topic command *
                else if (strcmp(command, "/topic") == 0)
                {
                    pthread_mutex_lock(&room_topics_mutex);
                    int topic_idx = find_or_create_room_info_index(cli->room);

                    if (strlen(argument) == 0)
                    { // No argument: Show topic
                        if (topic_idx != -1)
                        {
                            sprintf(message_buffer, "SYSTEM: Topic for '%s' is: %s\n", cli->room, room_topics[topic_idx].topic);
                        }
                        else
                        {
                            sprintf(message_buffer, "SYSTEM: Topic for '%s': %s\n", cli->room, DEFAULT_TOPIC);
                        }
                        pthread_mutex_unlock(&room_topics_mutex);
                        send_to_client(cli->sockfd, message_buffer);
                    }
                    else
                    { // Argument provided: Set topic
                        if (topic_idx != -1)
                        {
                            if (strlen(argument) < TOPIC_LEN)
                            {
                                strncpy(room_topics[topic_idx].topic, argument, TOPIC_LEN - 1);
                                room_topics[topic_idx].topic[TOPIC_LEN - 1] = '\0';
                                pthread_mutex_unlock(&room_topics_mutex); // Unlock before broadcasting

                                // Notify room
                                sprintf(message_buffer, "SYSTEM: User '%s' changed the topic for '%s' to: %s\n",
                                        cli->nickname, cli->room, argument);
                                broadcast_system_room(message_buffer, cli->room);

                                // Optional: Send confirmation back to user
                                // sprintf(message_buffer, "SYSTEM: Topic set for '%s'.\n", cli->room);
                                // send_to_client(cli->sockfd, message_buffer);
                            }
                            else
                            {
                                pthread_mutex_unlock(&room_topics_mutex);
                                sprintf(message_buffer, "SYSTEM: Error: Topic is too long (max %d chars).\n", TOPIC_LEN - 1);
                                send_to_client(cli->sockfd, message_buffer);
                            }
                        }
                        else
                        {
                            pthread_mutex_unlock(&room_topics_mutex);
                            sprintf(message_buffer, "SYSTEM: Error: Could not set topic (server room limit reached?).\n");
                            send_to_client(cli->sockfd, message_buffer);
                        }
                    }
                }
                // * END NEW: /topic command *
                else if (strcmp(command, "/nick") == 0)
                {
                    // [/nick] code as before...
                    if (strlen(argument) > 0 && strlen(argument) < NICK_LEN)
                    {
                        char old_nick[NICK_LEN];
                        strncpy(old_nick, cli->nickname, NICK_LEN);
                        old_nick[NICK_LEN - 1] = '\0';
                        // TODO: Add nick uniqueness check
                        pthread_mutex_lock(&clients_mutex);
                        strncpy(cli->nickname, argument, NICK_LEN - 1);
                        cli->nickname[NICK_LEN - 1] = '\0';
                        pthread_mutex_unlock(&clients_mutex);
                        sprintf(message_buffer, "SYSTEM: Your nick changed to '%s'.\n", cli->nickname);
                        send_to_client(cli->sockfd, message_buffer);
                        sprintf(message_buffer, "SYSTEM: User '%s' is now known as '%s' in room '%s'.\n", old_nick, cli->nickname, cli->room);
                        broadcast_system_room(message_buffer, cli->room);
                        printf("INFO: Client %s:%d changed nick to '%s'\n", inet_ntoa(cli->address.sin_addr), ntohs(cli->address.sin_port), cli->nickname);
                    }
                    else
                    {
                        sprintf(message_buffer, "SYSTEM: Invalid nick provided.\n");
                        send_to_client(cli->sockfd, message_buffer);
                    }
                }
                else if (strcmp(command, "/join") == 0)
                {
                    // [/join] code as before... + send topic
                    if (strlen(argument) > 0 && strlen(argument) < ROOM_LEN)
                    {
                        if (strcmp(cli->room, argument) == 0)
                        {
                            sprintf(message_buffer, "SYSTEM: You are already in room '%s'.\n", cli->room);
                            send_to_client(cli->sockfd, message_buffer);
                        }
                        else
                        {
                            char old_room[ROOM_LEN];
                            strncpy(old_room, cli->room, ROOM_LEN);
                            old_room[ROOM_LEN - 1] = '\0';
                            sprintf(message_buffer, "SYSTEM: User '%s' has left room '%s'.\n", cli->nickname, old_room);
                            broadcast_system_room(message_buffer, old_room);
                            pthread_mutex_lock(&clients_mutex);
                            strncpy(cli->room, argument, ROOM_LEN - 1);
                            cli->room[ROOM_LEN - 1] = '\0';
                            pthread_mutex_unlock(&clients_mutex);
                            sprintf(message_buffer, "SYSTEM: You joined room '%s'.\n", cli->room);
                            send_to_client(cli->sockfd, message_buffer);

                            // * NEW: Send topic of the new room *
                            pthread_mutex_lock(&room_topics_mutex);
                            int topic_idx = find_or_create_room_info_index(cli->room);
                            if (topic_idx != -1)
                            {
                                sprintf(message_buffer, "SYSTEM: Topic for '%s' is: %s\n", cli->room, room_topics[topic_idx].topic);
                            }
                            else
                            {
                                sprintf(message_buffer, "SYSTEM: Topic for '%s': %s\n", cli->room, DEFAULT_TOPIC);
                            }
                            pthread_mutex_unlock(&room_topics_mutex);
                            send_to_client(cli->sockfd, message_buffer);
                            // * END NEW *

                            sprintf(message_buffer, "SYSTEM: User '%s' has joined room '%s'.\n", cli->nickname, cli->room);
                            broadcast_system_room(message_buffer, cli->room);
                            printf("INFO: Client %s (%s:%d) joined room '%s'\n", cli->nickname, inet_ntoa(cli->address.sin_addr), ntohs(cli->address.sin_port), cli->room);
                        }
                    }
                    else
                    {
                        sprintf(message_buffer, "SYSTEM: Invalid room name provided.\n");
                        send_to_client(cli->sockfd, message_buffer);
                    }
                }
                else
                {
                    sprintf(message_buffer, "SYSTEM: Unknown command: %s\n", buffer);
                    send_to_client(cli->sockfd, message_buffer);
                }
            }
            else
            { // sending message to room
                snprintf(message_buffer, sizeof(message_buffer), "[%s] %s: %s\n", cli->room, cli->nickname, buffer);
                broadcast_room(message_buffer, cli->sockfd, cli->room);
            }
        }
        else if (receive == 0 || (receive < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
        {
            sprintf(message_buffer, "SYSTEM: User '%s' has left the chat from room '%s'.\n", cli->nickname, cli->room);
            broadcast_system_room(message_buffer, cli->room);
            printf("INFO: Client disconnected: %s (%s:%d)\n", cli->nickname, inet_ntoa(cli->address.sin_addr), ntohs(cli->address.sin_port));
            leave_flag = 1;
        }
        else
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                perror("ERROR: recv failed");
                leave_flag = 1;
            }
        }
    }

    close(cli->sockfd);
    remove_client(cli->sockfd);
    free(cli);
    pthread_detach(pthread_self());
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    { /* ... usage message ... */
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    int port = atoi(argv[1]);
    int listen_sockfd, conn_sockfd;
    struct sockaddr_in serv_addr, cli_addr;
    socklen_t clilen = sizeof(cli_addr);

    // * NEW: Initialize room_topics array *
    pthread_mutex_lock(&room_topics_mutex);
    for (int i = 0; i < MAX_ROOMS; ++i)
    {
        memset(room_topics[i].name, 0, ROOM_LEN);
        memset(room_topics[i].topic, 0, TOPIC_LEN);
    }
    // Optionally pre-define Lobby topic
    int lobby_idx = find_or_create_room_info_index(DEFAULT_ROOM);

    pthread_mutex_unlock(&room_topics_mutex);

    for (int i = 0; i < MAX_CLIENTS; ++i)
        clients[i] = NULL;

    listen_sockfd = socket(AF_INET, SOCK_STREAM, 0); // AF_INET for IPv4, SOCK_STREAM for TCP, 0 for default protocol should return >0 in order to create a socket
    if (listen_sockfd < 0)
    {
        perror("ERROR opening socket");
        exit(EXIT_FAILURE);
    }
    int opt = 1;
    /**
     * @brief Sets the SO_REUSEADDR socket option.
     *
     * This option allows the socket to be bound to an address that is already in use,
     * particularly when the previous socket is in the TIME_WAIT state. This is useful
     * for quickly restarting a server without waiting for the TIME_WAIT period to expire.
     * If setting the option fails, an error is typically handled (though the error handling
     * part is not included in this specific snippet).
     *
     * @param listen_sockfd The file descriptor of the listening socket.
     * @param opt A variable (typically an integer set to 1) to enable the SO_REUSEADDR option.
     * @return Returns a negative value if setsockopt fails, 0 on success.
     */
    if (setsockopt(listen_sockfd, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt)) < 0)
    {
        perror("ERROR on setsockopt");
    }
    memset((char *)&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);
    if (bind(listen_sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("ERROR on binding");
        close(listen_sockfd);
        exit(EXIT_FAILURE);
    }
    if (listen(listen_sockfd, 5) < 0)
    {
        perror("ERROR on listen");
        close(listen_sockfd);
        exit(EXIT_FAILURE);
    }

    printf("INFO: Server listening on port %d\n", port);

    while (1)
    {
        conn_sockfd = accept(listen_sockfd, (struct sockaddr *)&cli_addr, &clilen);
        if (conn_sockfd < 0)
        {
            perror("ERROR on accept");
            continue;
        }

        pthread_mutex_lock(&clients_mutex); // Check client count under lock
        if (client_count >= MAX_CLIENTS)
        {
            pthread_mutex_unlock(&clients_mutex);
            printf("WARN: Max clients reached. Rejecting %s:%d\n", inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port));
            send_to_client(conn_sockfd, "SYSTEM: Server is full.\n");
            close(conn_sockfd);
            continue;
        }
        pthread_mutex_unlock(&clients_mutex); // Unlock before malloc

        client_t *cli = (client_t *)malloc(sizeof(client_t));
        if (!cli)
        {
            perror("ERROR: malloc failed");
            close(conn_sockfd);
            continue;
        }
        cli->address = cli_addr;
        cli->sockfd = conn_sockfd;
        sprintf(cli->nickname, "User%d", conn_sockfd);
        strcpy(cli->room, DEFAULT_ROOM);

        add_client(cli); 
        if (pthread_create(&cli->tid, NULL, &handle_client, (void *)cli) != 0)
        {
            perror("ERROR creating thread");
            remove_client(cli->sockfd);
            free(cli);
            close(conn_sockfd);
        }
    }

    close(listen_sockfd);
    pthread_mutex_destroy(&clients_mutex);
    pthread_mutex_destroy(&room_topics_mutex);

    return 0;
}