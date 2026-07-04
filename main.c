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

    ssize_t sent=send(sockfd,request,strlen(request),0);

    if (sent==-1){
        perror("send");
        close(sockfd);
        return 1;
    }

    // printf("send %zd bytes\n",sent);

    char buf[4096];
    
    ssize_t received = recv(sockfd,buf,sizeof(buf)-1,0);

    if (received == -1){
        perror("recv");
        close(sockfd);
        return 1;
    }

    buf[received]='\0';
    
    printf("---- response ----\n");
    printf("%s\n", buf);
    printf("---- end response ----\n");


    close(sockfd);

    return 0;
}