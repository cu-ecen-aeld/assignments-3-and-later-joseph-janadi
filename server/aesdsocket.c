#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <netdb.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

void handler(int sig);

int server_fd = -1;
int client_fd = -1;
const char *data_file = "/var/tmp/aesdsocketdata";
int ret;

int main(int argc, char *argv[])
{
    /* Set signal handler for SIGINT and SIGTERM */
    struct sigaction act;
    act.sa_handler = handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    ret = sigaction(SIGINT, &act, NULL);
    if (ret == -1) { perror("Error"); return -1; }
    ret = sigaction(SIGTERM, &act, NULL);
    if (ret == -1) { perror("Error"); return -1; }

    /* Create socket */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) { perror("socket"); return -1; }

    /* Bind socket to port */
    char *port_str = "9000";
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res;
    ret = getaddrinfo(NULL, port_str, &hints, &res);
    if (ret != 0) {
        printf("getaddrinfo: %s\n", gai_strerror(ret));
        return -1;
    }
    ret = bind(server_fd, res->ai_addr, res->ai_addrlen);
    if (ret == -1) { perror("bind"); return -1; }

    /* Open or create data file */
    int datafd;
    datafd = open(data_file, O_RDWR | O_APPEND | O_CREAT);
    if (datafd == -1) { perror("open"); return -1; }

    /* Listen for connections */
    int backlog = 1;
    ret = listen(server_fd, backlog);
    if (ret == -1) { perror("listen"); return -1; }

    size_t packet_buf_len = 500;
    char *packet_buf = (char *)malloc(packet_buf_len);
    size_t packet_len = 0;

    struct sockaddr_in accepted_sockaddr;
    socklen_t addrlen = sizeof(accepted_sockaddr);
    ssize_t nread;
    const size_t BUF_SIZE = 500;
    char buf[BUF_SIZE];
    char *newline_ptr;
    ptrdiff_t newline_pos;
    size_t str_len;
    ssize_t nwritten;
    ssize_t nsent;
    struct stat statbuf;
    off_t offset = 0;
    /* Accept connections until SIGINT or SIGTERM */
    while (1) {
        /* Accept next connection */
        client_fd = accept(server_fd, (struct sockaddr *)&accepted_sockaddr, &addrlen);
        if (client_fd == -1) { perror("accept"); return -1; }
        syslog(LOG_DEBUG, "Accepted connection from %u", accepted_sockaddr.sin_addr.s_addr);

        /* Read from socket until closed or no more data */
        while (1) {
            /* Peek at stream contents */
            nread = recv(client_fd, buf, BUF_SIZE, MSG_PEEK);
            if (nread == -1) { perror("recv"); return -1; }
            /* If socket closed or no more data, break */
            else if (nread == 0) { break; }
            /* Find newline character */
            newline_ptr = strchr(buf, '\n');
            newline_pos = newline_ptr == NULL ? nread - 1 : newline_ptr - buf;
            str_len = newline_pos + 1;
            /* Increase packet buffer size if exceeded */
            if ((packet_len + str_len) > packet_buf_len) {
                size_t new_len = packet_buf_len + str_len;
                char *tmp = realloc(packet_buf, new_len);
                if (tmp == NULL) {
                    printf("realloc failed\n");
                    return -1;
                }
                packet_buf = tmp;
                packet_buf_len = new_len;
            }
            /* Append str_len from stream to packet buffer */
            nread = recv(client_fd, packet_buf + packet_len, str_len, 0);
            if (nread == -1) { perror("recv"); return -1; }
            packet_len += str_len;
            /* If packet not complete, loop back to receive again */
            if (newline_ptr == NULL)
                continue;
            /* If packet complete, write to data file */
            nwritten = write(datafd, packet_buf, packet_len);
            if (nwritten == -1) { perror("write"); return -1; }
        }

        /* Return contents of data file to client */
        ret = fstat(datafd, &statbuf);
        if (ret == -1) { perror("stat"); return -1; }
        nsent = sendfile(client_fd, datafd, &offset, statbuf.st_size);
        if (nsent == -1) { perror("sendfile"); return -1; }

        /* Reset packet_len */
        packet_len = 0;

        /* Close connection */
        ret = close(client_fd);
        if (ret == -1 ) { perror("close"); return -1; }
        syslog(LOG_DEBUG, "Closed connection from %u", accepted_sockaddr.sin_addr.s_addr);
    }

    freeaddrinfo(res);
}

void handler(int sig)
{
    /* Complete any open connections */
    // TODO

    /* Close any open sockets */
    close(client_fd);

    /* Delete data file */
    ret = remove(data_file);
    if (ret == -1) { perror("remove"); exit(EXIT_FAILURE); }
    syslog(LOG_DEBUG, "Caught signal, exiting");

    exit(EXIT_SUCCESS);
}
