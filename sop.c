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
    int* epoll_fd;
    struct epoll_event event;

    int fd; // the file descriptor associated with the user
    int is_free; // denotes whether this context field is taken
    int logged_in; // 1 - user logged in; 0 - otherwise
    char username[MAX_USERNAME_LENGTH + 1];

    int input_offset;
    char input_buffer[MAX_MESSAGE_SIZE];

    int output_offset; // where to write new data into the buffer
    int write_offset; // where to read next for sending
    int occupied_space; // how much data total is in the buffer
    char output_buffer[MAX_MESSAGE_SIZE];

    //int color_flag;
}context_t;

int safe_write(char* buf, context_t* contexts, int pos, int* logged_users);
void flood(char* buf, context_t* contexts, int pos ,int* logged_users);
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

void flood(char* buf, context_t* contexts, int pos ,int* logged_users)
{
    for(int i = 0; i < MAX_CLIENTS + 1; i++)
    {
        if(!contexts[i].is_free && contexts[i].logged_in && contexts[i].fd != contexts[pos].fd)
        {
            int ret = safe_write(buf, contexts, i, logged_users);
        }
    }
}

int circuclar_strlen(char* buf, int offset){
    int length = 0;
    while(buf[offset] != '\0')
    {
        offset = (offset + 1)%MAX_MESSAGE_SIZE;
        length++;
    }
    return length;
}

void safe_copy(char* buf, int length, context_t* contexts, int pos)
{
    int chunk_length = (MAX_MESSAGE_SIZE - contexts[pos].output_offset <= length) ? (MAX_MESSAGE_SIZE - contexts[pos].output_offset) : length;
    strncpy(contexts[pos].output_buffer + contexts[pos].output_offset, buf, chunk_length);
    contexts[pos].output_offset = (contexts[pos].output_offset + chunk_length) % MAX_MESSAGE_SIZE;

    int remaining = length - chunk_length;
    if(remaining != 0)
    {
        strncpy(contexts[pos].output_buffer + contexts[pos].output_offset, buf + chunk_length, remaining);
        contexts[pos].output_offset += (contexts[pos].output_offset + remaining) % MAX_MESSAGE_SIZE;
    }
}

int circular_write(/*char* buf,*/ context_t* contexts, int pos, int* logged_users)
{
    int length = circuclar_strlen(contexts[pos].output_buffer, contexts[pos].write_offset);
    int chunk_length = ( MAX_MESSAGE_SIZE - contexts[pos].write_offset <= length )? (MAX_MESSAGE_SIZE - contexts[pos].write_offset ): length;
    return write(contexts[pos].fd, (contexts[pos].output_buffer + contexts[pos].write_offset), chunk_length);
    //contexts[pos].write_offset += chunk_length;
}

int atomic_write(/*char* buf,*/ context_t* contexts, int pos, int* logged_users)
{
    //if(contexts[pos].color_flag) set_color(contexts[pos].fd, SOP_GREEN);
    int cummulative_write_bytes = 0;
    int length = circuclar_strlen(contexts[pos].output_buffer, contexts[pos].write_offset);
    while(cummulative_write_bytes != length){
        //int write_bytes = write(contexts[pos].fd, (contexts[pos].output_buffer + contexts[pos].write_offset), strlen(buf));
        int write_bytes = circular_write(contexts, pos, logged_users);
        if(write_bytes <= 0)
        {
            if(errno == EPIPE){ 
                close_connection(contexts, pos, logged_users, 0);
            }
            return cummulative_write_bytes;
        }
        else if(write_bytes > 0){
            contexts[pos].occupied_space -= write_bytes;
            contexts[pos].write_offset = (contexts[pos].write_offset + write_bytes) % MAX_MESSAGE_SIZE;
        }
        cummulative_write_bytes += write_bytes;
    }

    contexts[pos].occupied_space --; // we exclude '/0' character
    contexts[pos].write_offset = (contexts[pos].write_offset + 1) % MAX_MESSAGE_SIZE;
    //if(contexts[pos].color_flag) reset_color(contexts[pos].fd);
    return cummulative_write_bytes;
}

int safe_write(char* buf, context_t* contexts, int pos, int* logged_users/*, int set_color_flag*/)
{
    if(buf != NULL){
        int occupied_space = contexts[pos].occupied_space + strlen(buf) + 1; // + 1 for \0
        if(occupied_space < MAX_MESSAGE_SIZE){
            safe_copy(buf, strlen(buf) + 1, contexts, pos);
            contexts[pos].occupied_space += strlen(buf) + 1;
        }else{
            fprintf(stdout, "[Server]: [%d]'s output buffer is full, message is dropped\n", contexts[pos].fd);
            return -1;
        }
    }

    int cummulative_write_bytes = 0;
    while(contexts[pos].occupied_space != 0){
        int write_bytes = atomic_write(contexts, pos, logged_users);
        if(write_bytes <= 0){
            break;
        }
        cummulative_write_bytes += write_bytes;
    }

    if(contexts[pos].occupied_space == 0)
    {
        if((contexts[pos].event.events & EPOLLOUT) != 0) contexts[pos].event.events &= ~EPOLLOUT;
        if(epoll_ctl(*(contexts[pos].epoll_fd), EPOLL_CTL_MOD, contexts[pos].fd, &contexts[pos].event) < 0)
            ERR("epoll_ctl");
        contexts[pos].output_buffer[contexts[pos].write_offset] = '\0';
    }
    else{
        if((contexts[pos].event.events & EPOLLOUT) == 0) contexts[pos].event.events|= EPOLLOUT;
        if(epoll_ctl(*(contexts[pos].epoll_fd), EPOLL_CTL_MOD, contexts[pos].fd, &contexts[pos].event) < 0)
            ERR("epoll_ctl");
    }

    return cummulative_write_bytes;
}

void send_error_message(context_t* contexts, int pos, int client_fd, char* client_username, int* logged_users)
{
    char buffer[2*MAX_MESSAGE_SIZE + 1];
    fprintf(stdout, "[Server]: [%d] known as [%s] has disconnected\n", client_fd, client_username);
    snprintf(buffer, 2*MAX_MESSAGE_SIZE - 1, "[Admin]: User [%s] is gone!\n", client_username);
    flood(buffer, contexts, pos, logged_users);
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

    if(epoll_ctl(*(contexts[pos].epoll_fd), EPOLL_CTL_DEL, client_fd, NULL) < 0)
        ERR("epoll_ctl");

    close(contexts[pos].fd);
    contexts[pos].input_offset = 0;
    contexts[pos].output_offset = 0;
    contexts[pos].fd = -1;
    contexts[pos].logged_in = 0;
    contexts[pos].is_free = 1;
    contexts[pos].write_offset = 0;
    contexts[pos].occupied_space = 0;
    //contexts[pos].color_flag = 0;
    memset(contexts[pos].input_buffer, 0, MAX_MESSAGE_SIZE);
    memset(contexts[pos].output_buffer,0, MAX_MESSAGE_SIZE);
    memset(contexts[pos].username, 0, MAX_USERNAME_LENGTH + 1);

    if(flood_message){
        send_error_message(contexts, pos, client_fd, client_username, logged_users);
    }

    return;
}

void handle_client(context_t* contexts, int client_fd, uint32_t event_type, int* logged_users)
{
    int pos = tryadd_context(contexts, MAX_CLIENTS + 1, client_fd);
    char buffer[2*MAX_MESSAGE_SIZE];

    //contexts[pos].event = event;
    if((event_type & EPOLLRDHUP) != 0){ 
        close_connection(contexts, pos, logged_users, 1);
        return;
    }
    else if((event_type & EPOLLOUT) != 0){
        safe_write(NULL, contexts, pos, logged_users); // means flush the buffer
        return;
    }

    if(!contexts[pos].logged_in)
    {
        for(; contexts[pos].input_offset <= MAX_USERNAME_LENGTH; contexts[pos].input_offset++)
        {
            if(contexts[pos].input_offset == MAX_USERNAME_LENGTH)
            {
                contexts[pos].username[MAX_USERNAME_LENGTH] = '\0';
                contexts[pos].logged_in = 1;
                contexts[pos].input_offset = 0;
                break;
            }
            int read_bytes = read(client_fd, (contexts[pos].username + contexts[pos].input_offset), 1);
            if(read_bytes < 0 && errno != EAGAIN)
            {
                close_connection(contexts, pos,logged_users,0);
                break;
            }
            else if(read_bytes < 0 && errno == EAGAIN) break;
            else if(read_bytes == 1 && contexts[pos].username[contexts[pos].input_offset] == '\n')
            {
                contexts[pos].username[contexts[pos].input_offset] = '\0';
                contexts[pos].logged_in = 1;
                contexts[pos].input_offset = 0;
                break;
            }
        }
        if(contexts[pos].is_free || !contexts[pos].logged_in) return;
        else
        {
            (*logged_users)++;
            fprintf(stdout, "[Server]: [%d] logged in as [%s]\n", client_fd, contexts[pos].username);
            if(*logged_users == 1)
            {
                char buf[] = "[Admin]: You're the first one here!\n";
                int ret = safe_write(buf, contexts, pos, logged_users);
                if(ret == -1) return;
            }
            else
            {
                char buf[MAX_USERNAME_LENGTH + 3] = "[Admin]: Current users:\n";
                int ret = safe_write(buf, contexts, pos, logged_users);
                if(ret == -1) return;
                for(int i = 0; i < MAX_CLIENTS + 1; i++)
                {
                    if(contexts[i].logged_in && contexts[i].fd != contexts[pos].fd)
                    {
                        snprintf(buf, MAX_USERNAME_LENGTH + 2, "%s\n", contexts[i].username);
                        ret = safe_write(buf, contexts, pos, logged_users);
                        if(ret == -1) return;
                    }
                }
                snprintf(buffer, MAX_MESSAGE_SIZE - 1, "[Admin]: User %s logged in\n", contexts[pos].username);
                flood(buffer, contexts, pos, logged_users);
            }
        }
    }
    if(contexts[pos].logged_in)
    {
        for(;contexts[pos].input_offset < MAX_MESSAGE_SIZE; contexts[pos].input_offset++)
        {
            int read_bytes = read(client_fd, (contexts[pos].input_buffer + contexts[pos].input_offset), 1);
            if(read_bytes < 0 && errno != EAGAIN)
            {
                close_connection(contexts, pos, logged_users, 1);
                break;
            }
            else if(read_bytes < 0 && errno == EAGAIN)
            {
                break;
            }
            else if(read_bytes == 1 && contexts[pos].input_buffer[contexts[pos].input_offset] == '\n')
            {
                contexts[pos].input_buffer[contexts[pos].input_offset] = '\0';
                snprintf(buffer, 2*MAX_MESSAGE_SIZE - 1, "[%s]: %s\n", contexts[pos].username, contexts[pos].input_buffer);
                flood(buffer, contexts, pos, logged_users);
                contexts[pos].input_offset = 0;
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
    struct epoll_event event, events[MAX_CLIENTS + 1]; //+ 1 is to reserve a space for the socket itself
    event.events = EPOLLIN;

    for(int i = 0; i < MAX_CLIENTS + 1; i++)
    {
        contexts[i].is_free = 1;
        contexts[i].logged_in = 0;
        contexts[i].output_offset = 0;
        contexts[i].input_offset = 0;
        contexts[i].epoll_fd = &epoll_fd;
        contexts[i].fd = -1;
        contexts[i].write_offset = 0;
        contexts[i].occupied_space = 0;
        //contexts[i].color_flag = 0;
        memset(&contexts[i].event, 0, sizeof(contexts[i].event));
    }

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
                    int bytes_write = write(client_fd, buf, strlen(buf));
                    if(bytes_write < 0)
                    {
                        if(bytes_write != EPIPE)
                            ERR("write");
                        
                    }
                    close(client_fd);
                }
                else
                {
                    fprintf(stdout, "[Server]: Connected: [%d]\n", client_fd);
                    contexts[pos].event.data.ptr = &contexts[pos];
                    contexts[pos].event.events = EPOLLIN;
                    if((contexts[pos].event.events & EPOLLRDHUP) == 0)
                        contexts[pos].event.events |= EPOLLRDHUP;

                    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &contexts[pos].event) < 0)
                        ERR("epoll_ctl");
                    
                    char buf[] = "[Admin]: Please enter your username\n";
                    int bytes_write = safe_write(buf, contexts, pos, &logged_users);
                    char login_buf[] = "[Admin]: User logging in...\n";
                    flood(login_buf, contexts, socket_position, &logged_users);
                }
            }
            else
            {
                handle_client(contexts, ((context_t*)events[i].data.ptr)->fd, events[i].events, &logged_users);
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
