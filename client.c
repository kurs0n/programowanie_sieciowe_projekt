#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <signal.h> // For Ctrl+C handling

#define BUFFER_SIZE 2048
#define ROOM_LEN 32
#define NICK_LEN 32 // Assuming max nick length might be useful

// --- Global variables ---
volatile sig_atomic_t keep_running = 1;                 // Flag to control the loop
int sockfd = 0;                                         // Global socket descriptor
char current_room[ROOM_LEN] = "Lobby";                  // Store current room name, init with default
pthread_mutex_t room_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex to protect current_room

// Default room name - should match server's default
const char *DEFAULT_ROOM = "Lobby";

// --- ANSI Escape Codes ---
#define ANSI_CLEAR_LINE "\033[K" // Clears from cursor to end of line
#define ANSI_CURSOR_START "\r"   // Moves cursor to beginning of the line

// Signal handler function (e.g., Ctrl+C)
void intHandler(int dummy)
{
    (void)dummy; // Avoid warning about unused parameter
    keep_running = 0;
    printf("\nCtrl+C detected. Exiting...\n");
    // Close socket to unblock recv in receiver thread
    if (sockfd > 0)
    {
        shutdown(sockfd, SHUT_RDWR); // More graceful shutdown
        close(sockfd);
        sockfd = -1; // Mark as closed
    }
}

// Helper function to update the room safely
void update_room(const char *new_room)
{
    pthread_mutex_lock(&room_mutex);
    strncpy(current_room, new_room, ROOM_LEN - 1);
    current_room[ROOM_LEN - 1] = '\0';
    pthread_mutex_unlock(&room_mutex);
}

// Helper function to get the room safely
void get_room(char *buffer, size_t buffer_size)
{
    pthread_mutex_lock(&room_mutex);
    strncpy(buffer, current_room, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
    pthread_mutex_unlock(&room_mutex);
}

// Function to print the prompt
// It displays the current room name followed by ": " and flushes stdout.
void print_prompt()
{
    char room_name[ROOM_LEN];
    get_room(room_name, ROOM_LEN); // Safely get the current room name
    printf("[%s]: ", room_name);   // Print the prompt with the room name
    fflush(stdout);                // Ensure the prompt is displayed immediately
}

// --- Message receiving thread ---
void *receive_handler(void *arg)
{
    char buffer[BUFFER_SIZE];
    char temp_room_name[ROOM_LEN]; // For parsing
    int socket_desc = *(int *)arg;

    while (keep_running)
    {
        memset(buffer, 0, BUFFER_SIZE);
        /**
         * @brief Receives data from the specified socket.
         *
         * This line of code attempts to receive up to BUFFER_SIZE - 1 bytes of data
         * from the socket identified by socket_desc. The received data is stored
         * in the buffer. The 0 as the last argument indicates no special flags
         * are being used for the receive operation.
         *
         * The variable receive will store the number of bytes actually received.
         * If the recv call is successful, receive will be non-negative.
         * If receive is 0, it means the peer has performed an orderly shutdown.
         * If receive is -1, an error occurred during the receive operation.
         */
        int receive = recv(socket_desc, buffer, BUFFER_SIZE - 1, 0);

        if (receive > 0)
        {
            buffer[receive] = '\0';

            // Check for room change confirmation from server
            // Example format: "SYSTEM: You joined room 'NewRoom'."
            if (strncmp(buffer, "SYSTEM: You joined room '", 25) == 0)
            {
                char *start = buffer + 25;       // Point after the starting quote
                char *end = strchr(start, '\''); // Find the ending quote
                if (end)
                {
                    size_t len = end - start;
                    if (len < ROOM_LEN)
                    {
                        strncpy(temp_room_name, start, len);
                        temp_room_name[len] = '\0';
                        update_room(temp_room_name); // Update global room name
                    }
                }
            }

            // --- Redraw logic ---
            // 1. Move cursor to the beginning of the line
            // 2. Clear the line (in case user was typing)
            // 3. Print the received message
            // 4. Print the prompt for the next input
            printf("%s%s", ANSI_CURSOR_START, ANSI_CLEAR_LINE); // Move cursor, clear line
            printf("%s", buffer);                               // Print the received message (ensure server sends \n)
            print_prompt();                                     // Print the input prompt for the next line
        }
        else if (receive == 0)
        {
            if (keep_running)
            { // Avoid message if we initiated the exit
                printf("\n%s%sSYSTEM: Server disconnected.\n", ANSI_CURSOR_START, ANSI_CLEAR_LINE);
            }
            keep_running = 0; // Signal main thread to exit
            break;
        }
        else
        { // receive < 0
            if (errno == EINTR && keep_running)
            { // Interrupted by signal (like Ctrl+C handled by us)
                continue;
            }
            else if (keep_running)
            {                                                         // Only report error if we weren't expecting to stop
                printf("\n%s%s", ANSI_CURSOR_START, ANSI_CLEAR_LINE); // Clear line before error
                perror("ERROR: recv failed");
                keep_running = 0; // Signal main thread to exit
            }
            break;
        }
    }
    // Ensure prompt is printed if loop exits cleanly but unexpectedly
    // if (keep_running) {
    //    print_prompt();
    // }
    return NULL;
}

// --- Main client function ---
int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <server_ip> <port>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    char *server_ip = argv[1];
    int port = atoi(argv[2]);
    struct sockaddr_in serv_addr;
    pthread_t recv_tid;
    char message[BUFFER_SIZE];

    // Set initial room
    update_room(DEFAULT_ROOM);

    // Register SIGINT (Ctrl+C) handler
    signal(SIGINT, intHandler);

    // Create socket
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
    {
        perror("ERROR opening socket");
        exit(EXIT_FAILURE);
    }

    // Configure server address
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    // Convert IP address
    if (inet_pton(AF_INET, server_ip, &serv_addr.sin_addr) <= 0)
    {
        perror("ERROR invalid server IP address");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // Connect to server
    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        perror("ERROR connecting to server");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Connected to server %s:%d\n", server_ip, port);

    // Create thread for receiving messages from server
    if (pthread_create(&recv_tid, NULL, receive_handler, (void *)&sockfd) != 0)
    {
        perror("ERROR creating receiver thread");
        close(sockfd);
        exit(EXIT_FAILURE);
    }
    // Main loop for sending messages (reading from stdin)
    while (keep_running)
    {
        print_prompt(); // Print the prompt before waiting for input

        // Read user input from stdin
        if (fgets(message, BUFFER_SIZE, stdin) != NULL)
        {
            // If keep_running changed while waiting for fgets (e.g., Ctrl+C)
            if (!keep_running)
                break;

            // Remove \n added by fgets
            message[strcspn(message, "\n")] = 0;

            if (strlen(message) > 0)
            {
                // Check for client-side quit command first
                if (strcmp(message, "/quit") == 0)
                {
                    keep_running = 0; // Signal threads to stop
                    // Optionally send /quit to server, though closing socket often suffices
                    // write(sockfd, message, strlen(message));
                    break; // Exit the loop immediately
                }

                // Send message/command to server
                if (write(sockfd, message, strlen(message)) < 0)
                {
                    // Check errno after write failure
                    if (errno == EPIPE)
                    { // Server likely closed connection
                        if (keep_running)
                        { // Avoid message if we initiated the exit
                            printf("\n%s%sSYSTEM: Cannot send message. Server disconnected.\n", ANSI_CURSOR_START, ANSI_CLEAR_LINE);
                        }
                    }
                    else if (keep_running)
                    {
                        printf("\n%s%s", ANSI_CURSOR_START, ANSI_CLEAR_LINE); // Clear line before error
                        perror("ERROR writing to socket");
                    }
                    keep_running = 0; // Signal threads to stop
                }
            }
        }
        else
        {
            // Error reading stdin or EOF (Ctrl+D)
            if (feof(stdin))
            {
                if (keep_running)
                {
                    printf("\nEOF detected on stdin. Exiting...\n");
                }
            }
            else if (keep_running)
            {
                printf("\n%s%s", ANSI_CURSOR_START, ANSI_CLEAR_LINE); // Clear line before error
                perror("ERROR reading from stdin");
            }
            keep_running = 0; // Signal threads to stop
        }
    }

    // Cleanup
    if (sockfd > 0)
    { // Check if socket wasn't already closed by handler
        printf("\nDisconnecting...\n");
        // Gracefully shut down the socket connection for both reading and writing.
        // This sends a FIN packet to the server, indicating no more data will be sent or received.
        shutdown(sockfd, SHUT_RDWR);
        // Close the socket file descriptor, releasing associated system resources.
        close(sockfd);
    }

    pthread_join(recv_tid, NULL);       // Wait for receiver thread to finish
    pthread_mutex_destroy(&room_mutex); // Clean up mutex

    printf("Disconnected.\n");
    return 0;
}
