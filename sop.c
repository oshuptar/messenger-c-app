#include "common.h"

#define MAX_CLIENTS 4
#define MAX_USERNAME_LENGTH 32
#define MAX_MESSAGE_SIZE 64


void usage(char* program_name)
{
    fprintf(stderr, "Usage: \n");

    fprintf(stderr, "\t%s", program_name);
    set_color(2, SOP_PINK);
    fprintf(stderr, " port\n");

    fprintf(stderr, "\t  port");
    reset_color(2);
    fprintf(stderr, " - the port on which the server will run\n");

    exit(EXIT_FAILURE);
}

typedef struct context
{
    int fd; // the file descriptor associated with the user
    int is_free; // denotes whether this context field is taken
    int logged_in; // 1 - user logged in; 0 - otherwise
    char username[MAX_USERNAME_LENGTH + 1];

    int input_offset;
    char input_buffer[MAX_MESSAGE_SIZE];

    int output_offset;
    char output_buffer[MAX_MESSAGE_SIZE];
}context_t;

int safe_write(char* buf, context_t* contexts, int pos, int* logged_users, int set_color);
void flood(char* buf, context_t* contexts, int pos ,int* logged_users, int set_color);
void send_error_message(context_t* contexts, int pos, int client_fd, char* client_username, int*);
void close_connection(context_t* contexts, int pos, int* logged_users, int flood_message);

int tryadd_context(context_t* context, int context_size ,int fd) // adds or gets an existing context
{
    int position = -1;
    for(int i = 0; i < context_size; i++)
    {
        if(context[i].fd == fd)
            return i;
        if(context[i].is_free)
            position = i;
    }
    if(position != -1)
    {
        context[position].fd = fd;
        context[position].is_free = 0;
    }
    return position; 
}

void flood(char* buf, context_t* contexts, int pos ,int* logged_users, int set_color_flag)
{
    for(int i = 0; i < MAX_CLIENTS + 1; i++)
    {
        if(!contexts[i].is_free && contexts[i].logged_in && contexts[i].fd != contexts[pos].fd)
        {
            int ret = safe_write(buf, contexts, i, logged_users, set_color_flag);
            if(ret == -1){(*logged_users)--;}
        }
    }
}

int safe_write(char* buf, context_t* contexts, int pos, int* logged_users, int set_color_flag)
{
    if(set_color_flag) set_color(contexts[pos].fd, SOP_GREEN);
    int write_bytes = write(contexts[pos].fd, buf, strlen(buf) + 1);
    if(set_color_flag) reset_color(contexts[pos].fd);
    if(write_bytes < 0)
    {
        if(errno == EPIPE) close_connection(contexts, pos, logged_users, 0);
        return -1;
    }
    return 1;
}

void send_error_message(context_t* contexts, int pos, int client_fd, char* client_username, int* logged_users)
{
    char buffer[2*MAX_MESSAGE_SIZE + 1];
    fprintf(stdout, "[Server]: [%d] known as [%s] has disconnected\n", client_fd, client_username);
    snprintf(buffer, 2*MAX_MESSAGE_SIZE - 1, "[Admin]: User [%s] is gone!\n", client_username);
    flood(buffer, contexts, pos, logged_users, 0);
}

void close_connection(context_t* contexts, int pos, int* logged_users, int flood_message)
{
    int client_fd = contexts[pos].fd;
    char client_username[MAX_USERNAME_LENGTH + 1];
    strncpy(client_username, contexts[pos].username, MAX_USERNAME_LENGTH + 1);

    flood_message &= contexts[pos].logged_in;
    if(contexts[pos].logged_in){ 
        (*logged_users)--;
    }
    else {
        fprintf(stdout, "[Server]: [%d] failed to log in\n", contexts[pos].fd);
    }
    fprintf(stdout, "[Server]: [%d] active users left\n", *logged_users);

    close(contexts[pos].fd);
    contexts[pos].input_offset = 0;
    contexts[pos].output_offset = 0;
    contexts[pos].fd = -1;
    contexts[pos].logged_in = 0;
    contexts[pos].is_free = 1; 
    memset(contexts[pos].input_buffer, 0, MAX_MESSAGE_SIZE);
    memset(contexts[pos].output_buffer,0, MAX_MESSAGE_SIZE);
    memset(contexts[pos].username, 0, MAX_USERNAME_LENGTH + 1);

    if(flood_message){
        send_error_message(contexts, pos, client_fd, client_username, logged_users);
    }

    return;
}

void handle_client(context_t* contexts, int client_fd,uint32_t event_type, int* logged_users)
{
    int pos = tryadd_context(contexts, MAX_CLIENTS + 1, client_fd);
    //int client_fd = contexts[pos].fd;
    char buffer[2*MAX_MESSAGE_SIZE];

    if((event_type & EPOLLRDHUP) != 0){ 
        close_connection(contexts, pos, logged_users, 1);
        return;
    }
    if(!contexts[pos].logged_in)
    {
        for(; contexts[pos].output_offset <= MAX_USERNAME_LENGTH; contexts[pos].output_offset++)
        {
            if(contexts[pos].output_offset == MAX_USERNAME_LENGTH)
            {
                contexts[pos].username[MAX_USERNAME_LENGTH] = '\0';
                contexts[pos].logged_in = 1;
                contexts[pos].output_offset = 0;
                break;
            }
            int read_bytes = read(client_fd, (contexts[pos].username + contexts[pos].output_offset), 1);
            if(read_bytes < 0 && errno != EAGAIN)
            {
                close_connection(contexts, pos,logged_users,0);
                break;
            }
            else if(read_bytes < 0 && errno == EAGAIN)
            {
                break;
            }
            else if(read_bytes == 1 && contexts[pos].username[contexts[pos].output_offset] == '\n')
            {
                contexts[pos].username[contexts[pos].output_offset] = '\0';
                contexts[pos].logged_in = 1;
                contexts[pos].output_offset = 0;
                break;
            }
        }
        if(contexts[pos].is_free || !contexts[pos].logged_in)
            return;
        else
        {
            (*logged_users)++;
            fprintf(stdout, "[Server]: [%d] logged in as [%s]\n", client_fd, contexts[pos].username);
            if(*logged_users == 1)
            {
                char buf[] = "[Admin]: You're the first one here!\n";
                int ret = safe_write(buf, contexts, pos, logged_users, 0);
                if(ret == -1) return;
            }
            else
            {
                char buf[MAX_USERNAME_LENGTH + 3] = "[Admin]: Current users:\n";
                int ret = safe_write(buf, contexts, pos, logged_users, 0);
                if(ret == -1) return;
                for(int i = 0; i < MAX_CLIENTS + 1; i++)
                {
                    if(contexts[i].logged_in && contexts[i].fd != contexts[pos].fd)
                    {
                        snprintf(buf, MAX_USERNAME_LENGTH + 2, "%s\n", contexts[i].username);
                        ret = safe_write(buf, contexts, pos, logged_users, 0);
                        if(ret == -1) return;
                    }
                }
                snprintf(buffer, MAX_MESSAGE_SIZE - 1, "[Admin]: User %s logged in\n", contexts[pos].username);
                flood(buffer, contexts, pos, logged_users, 1);
            }
        }
    }
    if(contexts[pos].logged_in)
    {
        for(;contexts[pos].output_offset < MAX_MESSAGE_SIZE; contexts[pos].output_offset++)
        {
            int read_bytes = read(client_fd, (contexts[pos].input_buffer + contexts[pos].output_offset), 1);
            if(read_bytes < 0 && errno != EAGAIN)
            {
                close_connection(contexts, pos, logged_users, 1);
                break;
            }
            else if(read_bytes < 0 && errno == EAGAIN)
            {
                break;
            }
            else if(read_bytes == 1 && contexts[pos].input_buffer[contexts[pos].output_offset] == '\n')
            {
                contexts[pos].input_buffer[contexts[pos].output_offset] = '\0';
                snprintf(buffer, 2*MAX_MESSAGE_SIZE - 1, "[%s]: %s\n", contexts[pos].username, contexts[pos].input_buffer);
                flood(buffer, contexts, pos, logged_users, 0);
                contexts[pos].output_offset = 0;
                break;
            }
        }
    }
}


void server_work(int socket_fd)
{
    int logged_users = 0;

    int epoll_fd = epoll_create1(0);
    if(epoll_fd < 0)
        ERR("epoll_create");

    context_t contexts[MAX_CLIENTS + 1];
    for(int i = 0; i < MAX_CLIENTS + 1; i++)
    {
        contexts[i].is_free = 1;
        contexts[i].logged_in = 0;
        contexts[i].output_offset = 0;
        contexts[i].input_offset = 0;
    }
    struct epoll_event event, events[MAX_CLIENTS + 1]; //+ 1 is to reserve a space for the socket itself
    event.events = EPOLLIN;

    int socket_position = tryadd_context(contexts, MAX_CLIENTS + 1, socket_fd);
    if(socket_position == -1){ 
        fprintf(stdout, "[Server]: Error occured. Terminating\n");
        return;
    }

    event.data.ptr = &contexts[socket_position];
    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_fd, &event)<0)
        ERR("epoll_ctl");
    
    while(1){
        int no_of_events = epoll_wait(epoll_fd, events, MAX_CLIENTS + 1, -1);
        if(no_of_events < 0)
            ERR("epoll_wait");
        
        for(int i = 0; i < no_of_events; i++)
        {
            if(((context_t*)events[i].data.ptr)->fd == socket_fd)
            {
                int client_fd = add_new_client(socket_fd);
                if(client_fd == -1){
                    fprintf(stdout, "[Server]: Error occured when adding a client\n");
                    continue;
                }

                fcntl(client_fd, F_SETFL, O_NONBLOCK);
                int pos = tryadd_context(contexts, MAX_CLIENTS + 1, client_fd);
                if(pos == -1)
                {
                    char buf[] = "Server is full\n";
                    int bytes_write = write(client_fd, buf, strlen(buf) + 1);
                    if(bytes_write < 0)
                    {
                        if(bytes_write == EPIPE)
                        {
                            close_connection(contexts, pos, &logged_users, 0);
                        }
                        else
                            ERR("write");
                    }
                    else 
                        close_connection(contexts, pos, &logged_users, 0);
                }
                else
                {
                    fprintf(stdout, "[Server]: Connected: [%d]\n", client_fd);
                    event.data.ptr = &contexts[pos];
                    event.events |= EPOLLRDHUP;
                    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event) < 0)
                        ERR("epoll_ctl");
                    
                    char buf[] = "[Admin]: Please enter your username\n";
                    int bytes_write = write(client_fd, buf, strlen(buf) + 1);
                    if(bytes_write < 0)
                    {
                        if(bytes_write == EPIPE)
                        {
                            close_connection(contexts, pos, &logged_users, 0);
                        }
                        else
                            ERR("write");
                    }
                    char login_buf[] = "[Admin]: User logging in...\n";
                    flood(login_buf, contexts, socket_position, &logged_users, 0);
                }
            }
            else
            {
                handle_client(contexts, ((context_t*)events[i].data.ptr)->fd, events[i].events , &logged_users);
            }
        }
    }
    close(epoll_fd);
}

int main(int argc, char** argv) 
{
    if(argc != 2)
        usage(argv[0]); 
    
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGPIPE);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    int port = atoi(argv[1]);
    if(port <= 1023 || port >= 65535)
        usage(argv[0]);

    int socket_fd = bind_tcp_socket((uint16_t)port, MAX_CLIENTS);
    fcntl(socket_fd, F_SETFL, O_NONBLOCK);

    fprintf(stdout, "[Server]: Server starting up..\n");
    server_work(socket_fd);

    fprintf(stdout, "[Server]: terminating\n");
    close(socket_fd); // do not forget about closing teh resources
    return EXIT_SUCCESS;
}
