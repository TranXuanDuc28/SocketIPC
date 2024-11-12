#include <sys/socket.h>
#include <sys/un.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>

#define SOCKET_PATH "/tmp/chat_socket"
#define BUFFER_SIZE 256
#define MAX_USERNAME_LENGTH 100
#define MAX_PASSWORD_LENGTH 100

void get_current_time(char *buffer, size_t buffer_size)
{
    time_t rawtime;
    struct tm *timeinfo;
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(buffer, buffer_size, "%Y-%m-%d %H:%M:%S", timeinfo);
}

void receive_messages(int socket_fd, FILE *output_file)
{
    char buffer[BUFFER_SIZE];
    int bytes_received;
    char time_str[64];

    while ((bytes_received = recv(socket_fd, buffer, BUFFER_SIZE, 0)) > 0)
    {
        buffer[bytes_received] = '\0';

        get_current_time(time_str, sizeof(time_str));
        fprintf(output_file, "[%s]: %s\n", time_str, buffer);
        fflush(output_file);

        printf("%s\n", buffer);
    }
}

void listen_stdin(int socket_fd)
{
    char buffer[BUFFER_SIZE];

    while (1)
    {
        if (fgets(buffer, BUFFER_SIZE, stdin) != NULL)
        {
            send(socket_fd, buffer, strlen(buffer), 0);
        }
    }
}

int authenticate(int client_fd, int is_registration, char *username, char *password)
{
    char buffer[BUFFER_SIZE];

    // Nhập username và password từ bàn phím
    printf("Enter username: ");
    fgets(username, MAX_USERNAME_LENGTH, stdin);
    username[strcspn(username, "\n")] = '\0'; // Xóa ký tự xuống dòng

    printf("Enter password: ");
    fgets(password, MAX_PASSWORD_LENGTH, stdin);
    password[strcspn(password, "\n")] = '\0'; // Xóa ký tự xuống dòng

    // Gửi thông điệp xác thực hoặc đăng ký
    snprintf(buffer, BUFFER_SIZE, "%.*s|%.*s|%d",
             MAX_USERNAME_LENGTH - 1, username,
             MAX_PASSWORD_LENGTH - 1, password,
             is_registration);

    send(client_fd, buffer, strlen(buffer), 0);

    // Nhận phản hồi xác thực hoặc đăng ký
    int bytes_received = recv(client_fd, buffer, BUFFER_SIZE, 0);
    buffer[bytes_received] = '\0';

    if (strncmp(buffer, "SUCCESS", 7) == 0)
    {
        if (is_registration == 1)
            printf("Registration successful. Please login now.\n");
        else
            printf("Login successful. Connected to chat server.\n");

        return 1;
    }
    else
    {
        printf("Authentication/Registration failed: %s\n", buffer);
        return 0;
    }
}

int main(int argc, char *argv[])
{
    int client_fd;
    struct sockaddr_un addr;
    FILE *input_file;
    FILE *output_file;
    char username[MAX_USERNAME_LENGTH];
    char password[MAX_PASSWORD_LENGTH];
    int is_registration;

    if (argc < 3)
    {
        fprintf(stderr, "Usage: %s <input_file> <output_file>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *input_filename = argv[1];
    input_file = fopen(input_filename, "r");
    if (!input_file)
    {
        perror("Failed to open input file");
        exit(EXIT_FAILURE);
    }

    const char *output_filename = argv[2];
    output_file = fopen(output_filename, "a");
    if (!output_file)
    {
        perror("Failed to open output file");
        fclose(input_file);
        exit(EXIT_FAILURE);
    }

    client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_fd == -1)
    {
        perror("Socket failed");
        fclose(input_file);
        fclose(output_file);
        exit(EXIT_FAILURE);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (connect(client_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
    {
        perror("Connect failed");
        close(client_fd);
        fclose(input_file);
        fclose(output_file);
        exit(EXIT_FAILURE);
    }

    // Hỏi người dùng muốn đăng ký hay đăng nhập trước khi nhập thông tin
    while (1)
    {
        printf("Do you want to (1) Register or (2) Login? Enter 1 or 2: ");
        scanf("%d", &is_registration);
        getchar(); // Xóa ký tự xuống dòng còn lại trong buffer

        if (is_registration == 1)
        {
            // Thực hiện đăng ký
            if (authenticate(client_fd, is_registration, username, password))
            {

                continue;
            }
        }
        else if (is_registration == 2)
        {
            // Thực hiện đăng nhập
            if (authenticate(client_fd, is_registration, username, password))
                break; // Thoát vòng lặp sau khi đăng nhập thành công
        }
        else
        {
            printf("Invalid option. Please enter 1 or 2.\n");
        }
    }

    // Fork tiến trình để nhận tin nhắn
    pid_t receive_pid = fork();
    if (receive_pid == 0)
    {
        receive_messages(client_fd, output_file);
        exit(0);
    }

    // Fork tiến trình để gửi tin nhắn từ stdin
    pid_t stdin_pid = fork();
    if (stdin_pid == 0)
    {
        listen_stdin(client_fd);
        exit(0);
    }

    // Gửi nội dung từ input file
    char buffer[BUFFER_SIZE];
    while (fgets(buffer, BUFFER_SIZE, input_file) != NULL)
    {
        send(client_fd, buffer, strlen(buffer), 0);
        sleep(1);
    }

    fclose(input_file);

    printf("Finished sending input file. You can now type messages. Press Enter to send.\n");

    // Chờ các tiến trình con
    waitpid(receive_pid, NULL, 0);
    waitpid(stdin_pid, NULL, 0);

    fclose(output_file);
    close(client_fd);
    return 0;
}

