#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <sys/select.h>

#define SOCKET_PATH "/tmp/chat_socket"
#define BUFFER_SIZE 256
#define MAX_CLIENTS 10
#define USER_FILE "users.txt"

typedef struct {
    int fd;
    char username[BUFFER_SIZE];
} Client;

Client clients[MAX_CLIENTS] = {0};

// Lấy vị trí client theo file descriptor
int get_position_by_fd(int fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd == fd) {
            return i + 1;
        }
    }
    return -1;
}

// Kiểm tra người dùng trong file
bool user_exists(const char *username, const char *password) {
    FILE *file = fopen(USER_FILE, "r");
    if (!file) {
        perror("Could not open user file");
        return false;
    }

    char line[BUFFER_SIZE];
    while (fgets(line, sizeof(line), file)) {
        char file_user[BUFFER_SIZE], file_pass[BUFFER_SIZE];
        sscanf(line, "%s %s", file_user, file_pass);
        if (strcmp(file_user, username) == 0 && strcmp(file_pass, password) == 0) {
            fclose(file);
            return true;
        }
    }

    fclose(file);
    return false;
}

// Đăng ký người dùng mới
bool register_user(const char *username, const char *password) {
    if (user_exists(username, password)) {
        return false;
    }
    
    FILE *file = fopen(USER_FILE, "a");
    if (!file) {
        perror("Could not open user file for writing");
        return false;
    }
    
    fprintf(file, "%s %s\n", username, password);
    fclose(file);
    return true;
}

// Xử lý xác thực từ client
bool handle_authentication(int client_fd, const char *auth_message, char *username) {
    char password[BUFFER_SIZE];
    int is_registration;

    sscanf(auth_message, "%[^|]|%[^|]|%d", username, password, &is_registration);

    if (is_registration == 1) {
        // Đăng ký người dùng mới
        if (register_user(username, password)) {
            send(client_fd, "SUCCESS", strlen("SUCCESS"), 0);
            return true;
        } else {
            send(client_fd, "FAILURE: User already exists", strlen("FAILURE: User already exists"), 0);
            return false;
        }
    } else if (is_registration == 2) {
        // Đăng nhập người dùng đã có
        if (user_exists(username, password)) {
            send(client_fd, "SUCCESS", strlen("SUCCESS"), 0);
            return true;
        } else {
            send(client_fd, "FAILURE: Invalid credentials", strlen("FAILURE: Invalid credentials"), 0);
            return false;
        }
    } else {
        // Nếu giá trị không phải là 1 hoặc 2, gửi thông báo lỗi
        send(client_fd, "FAILURE: Invalid option", strlen("FAILURE: Invalid option"), 0);
        return false;
    }
}

// Tìm client theo tên người dùng
int find_client_fd(const char *username) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd > 0 && strcmp(clients[i].username, username) == 0) {
            return clients[i].fd;
        }
    }
    return -1;
}
// Lấy tên người dùng từ file descriptor
const char* get_username_by_fd(int fd) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd == fd) {
            return clients[i].username;
        }
    }
    return "Unknown"; // Trường hợp không tìm thấy
}

// Gửi tin nhắn riêng cho một client cụ thể
void send_private_message(int sender_fd, const char *recipient_name, const char *message) {
    char buffer[BUFFER_SIZE];
    char sender_name[BUFFER_SIZE];
    int recipient_fd = find_client_fd(recipient_name);

    // Lấy tên người gửi
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd == sender_fd) {
            strncpy(sender_name, clients[i].username, BUFFER_SIZE - 1);
            sender_name[BUFFER_SIZE - 1] = '\0';
            break;
        }
    }

    // Nếu không tìm thấy người nhận
    if (recipient_fd == -1) {
        snprintf(buffer, BUFFER_SIZE, "User %s not found.", recipient_name);
        send(sender_fd, buffer, strlen(buffer), 0);
        return;
    }

    // Xác định chiều dài tối đa cho message để vừa với buffer
    int sender_info_length = snprintf(buffer, BUFFER_SIZE, "Private from %s: ", sender_name);
    int max_message_length = BUFFER_SIZE - sender_info_length - 1;
    if (max_message_length > 0) {
        snprintf(buffer + sender_info_length, max_message_length + 1, "%s", message);
    }

    send(recipient_fd, buffer, strlen(buffer), 0);
}


// Gửi tin nhắn cho các client khác hoặc xử lý tin nhắn riêng
void broadcast_message(int sender_fd, const char *message) {
    char buffer[BUFFER_SIZE];
    const char *sender_name = get_username_by_fd(sender_fd);
    // Kiểm tra xem có phải tin nhắn riêng không (bắt đầu với '@')
    if (message[0] == '@') {
        char recipient_name[BUFFER_SIZE];
        const char *private_message = strchr(message, ' ');
        if (private_message == NULL) {
            snprintf(buffer, BUFFER_SIZE, "Invalid private message format. Use @<username> <message>.");
            send(sender_fd, buffer, strlen(buffer), 0);
            return;
        }
        
        // Tách tên người nhận và tin nhắn
        int name_length = private_message - (message + 1);
        strncpy(recipient_name, message + 1, name_length);
        recipient_name[name_length] = '\0';
        
        // Gửi tin nhắn riêng
        send_private_message(sender_fd, recipient_name, private_message + 1);
        return;
    }

    // Nếu là tin nhắn công khai, gửi đến tất cả các client
    for (int i = 0; i < MAX_CLIENTS; i++) {
        int fd = clients[i].fd;
        if (fd > 0 && fd != sender_fd) {
            snprintf(buffer, BUFFER_SIZE, "%s: %s", sender_name, message);
            send(fd, buffer, strlen(buffer), 0);
        }
    }
}

int main() {
    int server_fd, client_fd, max_fd, activity;
    struct sockaddr_un addr;
    fd_set read_fds;
    char buffer[BUFFER_SIZE];

    server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    unlink(SOCKET_PATH);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("Bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 5) == -1) {
        perror("Listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("Chat server is running on %s\n", SOCKET_PATH);

    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        max_fd = server_fd;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = clients[i].fd;
            if (fd > 0) {
                FD_SET(fd, &read_fds);
            }
            if (fd > max_fd) {
                max_fd = fd;
            }
        }

        activity = select(max_fd + 1, &read_fds, NULL, NULL, NULL);
        if ((activity < 0) && (errno != EINTR)) {
            perror("Select error");
        }

        if (FD_ISSET(server_fd, &read_fds)) {
            if ((client_fd = accept(server_fd, NULL, NULL)) < 0) {
                perror("Accept failed");
                exit(EXIT_FAILURE);
            }

            char username[BUFFER_SIZE];
            int bytes_read = recv(client_fd, buffer, BUFFER_SIZE, 0);
            buffer[bytes_read] = '\0';

            if (handle_authentication(client_fd, buffer, username)) {
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].fd == 0) {
                        clients[i].fd = client_fd;
                        strncpy(clients[i].username, username, BUFFER_SIZE - 1);
                        clients[i].username[BUFFER_SIZE - 1] = '\0';
                        printf("%s connected, fd: %d\n", username, client_fd);
                        break;
                    }
                }
            } else {
                close(client_fd);
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            int fd = clients[i].fd;
            if (fd > 0 && FD_ISSET(fd, &read_fds)) {
                int bytes_read = read(fd, buffer, BUFFER_SIZE);
                if (bytes_read == 0) {
                    close(fd);
                    clients[i].fd = 0;
                    printf("Client disconnected, fd: %d\n", get_position_by_fd(fd));
                    snprintf(buffer, BUFFER_SIZE, "Client %d disconnected", get_position_by_fd(fd));
                    broadcast_message(fd, buffer);
                } else {
                    buffer[bytes_read] = '\0';
                    broadcast_message(fd, buffer);
                }
            }
        }
    }

    close(server_fd);
    unlink(SOCKET_PATH);
    return 0;
}

