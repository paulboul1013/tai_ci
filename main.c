#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

#define PORT "80"

typedef struct {
    char scheme[16];
    char host[256];
    char path[1024];
} URL;

int parse_url(URL *u,const char *url){
    const char *scheme_end = strstr(url,"://");
    if (scheme_end == NULL) {
        fprintf(stderr,"URL must contain ://\n");
        return -1;
    }

    size_t scheme_len = scheme_end - url;
    memcpy(u->scheme,url,scheme_len);
    u->scheme[scheme_len] = '\0';

    assert(strcmp(u->scheme,"http")==0);

    const char *rest=scheme_end+3; //skip ://
    const char *slash = strchr(rest,'/'); // have /path
    
    if (slash == NULL) { //no /path
        strcpy(u->host,rest);
        strcpy(u->path,"/");
    } else{
        size_t host_len = slash-rest;
        memcpy(u->host,rest,host_len);
        u->host[host_len] ='\0';

        strcpy(u->path,slash);
    }

    return 0;
}


int connect_to_host(const char *host) {
    struct addrinfo hints; //for one domain name for multiple IP address
    struct addrinfo *result;
    struct addrinfo *rp;
    int sockfd = -1;

    memset(&hints,0,sizeof(hints));

    hints.ai_family = AF_INET; //ipv4
    hints.ai_socktype = SOCK_STREAM; // TCP stream
    hints.ai_protocol = IPPROTO_TCP; // TCP

    int err = getaddrinfo(host,PORT,&hints,&result);
    if (err!=0) {
        fprintf(stderr,"getaddrinfo: %s\n",gai_strerror(err));
        return -1;
    }

    for(rp=result; rp!=NULL ;rp=rp->ai_next) {
        sockfd = socket(rp->ai_family,rp->ai_socktype,rp->ai_protocol);
        if (sockfd==-1){
            continue;
        }

        if (connect(sockfd,rp->ai_addr,rp->ai_addrlen)==0){
            freeaddrinfo(result);
            return sockfd;
        }

        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(result);
    return -1;
}

int send_all(int sockfd,const char *data,size_t len) {
    size_t sent_total=0;

    while (sent_total < len) {
        ssize_t sent = send(sockfd,data+sent_total,len-sent_total,0);

        if (sent==-1) {
            perror("sending error");
            return -1;
        }

        if (sent==0){
            fprintf(stderr,"send returned 0\n");
            return -1;
        }

        sent_total += (size_t)sent;
    }

    return 0;
}

char *read_response(int sockfd,size_t *out_len) {
    size_t cap = 4096;
    size_t len = 0;

    char *response = malloc(cap+1);
    if (response==NULL){
        perror("malloc error");
        return NULL;
    }

    while(1) {
        if (len==cap){
            size_t new_cap = cap * 2;
            
            char *new_response = realloc(response,new_cap+1);
            if (new_response==NULL) {
                perror("realloc wrong");
                free(response);
                return NULL;
            }

            response = new_response;
            cap = new_cap;
        }

        ssize_t n = recv(sockfd,response+len,cap-len,0);
        
        if (n==-1){
            perror("recv wrong");
            free(response);
            return NULL;
        }

        if (n==0){
            break;
        }

        len+=(size_t)n;
    }

    response[len]='\0';
    *out_len = len;

    return response;
}

void parse_status_line(char *response) {
    char *line_end = strstr(response,"\r\n");

    if (line_end==NULL){
        fprintf(stderr,"invalid response: no status line\n");
        return;
    }

    *line_end = '\0'; //HTTP/1.1 200 OK\r\n to HTTP/1.1 200 OK \0\n for split by " "
    
    char *version = strtok(response," ");
    char *status = strtok(NULL," ");
    char *explanation = strtok(NULL," ");
    
    printf("version     = %s\n", version ? version : "");
    printf("status      = %s\n", status ? status : "");
    printf("explanation = %s\n", explanation ? explanation : "");

    *line_end = '\r'; //recover \0\n to \r\n
}

int main(int argc, char **argv) {

    if (argc !=2 ){
        fprintf(stderr,"usage: %s http://example.org/path\n",argv[0]);
        return 1;
    }

    URL url;
    if (parse_url(&url,argv[1]) !=0){
        return 1;
    }

    printf("scheme = %s\n",url.scheme);
    printf("host   = %s\n", url.host);
    printf("path   = %s\n", url.path);

    int sockfd = connect_to_host(url.host);
    if (sockfd==-1){
        fprintf(stderr,"connect failed\n");
        return 1;
    }

    printf("connect to %s:80\n",url.host);

    char request[2048];
    
    int n=snprintf(
        request,
        sizeof(request),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "\r\n",
        url.path,
        url.host
    );

    if (n<0 || n>=(int)sizeof(request)) {
        fprintf(stderr,"request too long\n");
        close(sockfd);
        return 1;
    }

    if (send_all(sockfd,request,strlen(request))!=0){
        close(sockfd);
        return 1;
    }

    printf("send %zd bytes\n",strlen(request));

    char buf[4096];
    
    ssize_t received = recv(sockfd,buf,sizeof(buf)-1,0);

    if (received == -1){
        perror("recv");
        close(sockfd);
        return 1;
    }

    buf[received]='\0';
    
    char *status_end = strstr(buf,"\r\n"); //find statusline

    if (status_end==NULL){
        fprintf(stderr,"invalid response: no status line\n");
        close(sockfd);
        return 1;
    }

    
    char *body = strstr(buf,"\r\n\r\n");
    if (body==NULL) {
        fprintf(stderr,"invalid response: no header/body separator\n");
        close(sockfd);
        return 1;
    }

    body += 4;
    printf("---- body ----\n");
    printf("%s\n", body);





    close(sockfd);

    return 0;
}