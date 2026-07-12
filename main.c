#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <strings.h>
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

    size_t line_len=(size_t)(line_end-response);

    char line[512];
    
    if (line_len>=sizeof(line)){
        fprintf(stderr,"status line too long\n");
        return;
    }

    memcpy(line,response,line_len);
    line[line_len] = '\0';

    char *version = line;
    
    char *space1 = strchr(line,' ');
    if (space1==NULL){
        fprintf(stderr,"invalid status line\n");
        return;
    }

    *space1 = '\0';

    char *status = space1+1;
    
    char *space2 = strchr(status,' ');
    if (space2==NULL) {
        fprintf(stderr,"invalid status line\n");
        return;
    }

    *space2='\0';

    char *explanation = space2+1;

    printf("version     = %s\n", version);
    printf("status      = %s\n", status);
    printf("explanation = %s\n", explanation);

}

void parse_headers(char *response) {
    char *status_end = strstr(response,"\r\n");
    char *headers_end = strstr(response,"\r\n\r\n");

    if (status_end == NULL || headers_end == NULL) {
        fprintf(stderr,"invalid response headers\n");
        return;
    }

    char *line = status_end + 2; //skip \r\n
    
    printf("---- headers ----\n");

    while (line < headers_end) {
        char *line_end = strstr(line,"\r\n");
        
        if (line_end == NULL || line_end > headers_end){
            break;
        }

        *line_end = '\0';

        char *colon = strchr(line,':');

        if (colon!=NULL){
            *colon='\0';
            
            char *header = line;
            char *value = colon+1;
            
            while (*value == ' ' || *value=='\t'){
                value++;
            }

            printf("%s = %s\n",header,value);

            if (strcasecmp(header,"Transfer-Encoding")==0){
                fprintf(stderr,"error: Transfer-Encoding is not supported\n");
            }

            if (strcasecmp(header,"Content-Encoding")==0){
                fprintf(stderr,"error: Content-Encoding is not supported\n");
            }

            *colon=':';
        }

        *line_end='\r';
        line = line_end+2;
    }

    printf("---- end headers ----\n");
}

void print_body(char *response) {
    char *body = strstr(response,"\r\n\r\n");
    
    if (body==NULL){
        fprintf(stderr,"invalid response: no body\n");
        return;
    }

    body += 4; //skip \r\n\r\n

    printf("---- body ----\n");
    printf("%s\n",body);
    printf("---- end body ----\n");
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

    printf("---- request ----\n");
    printf("%s\n",request);
    printf("---- end request ----\n");

    if (n<0 || n>=(int)sizeof(request)) {
        fprintf(stderr,"request too long\n");
        close(sockfd);
        return 1;
    }

    if (send_all(sockfd,request,strlen(request))!=0){
        close(sockfd);
        return 1;
    }

    size_t response_len=0;
    printf("request sent: %zu bytes\n",strlen(request));
    char *response = read_response(sockfd,&response_len);

    close(sockfd);

    if (response==NULL){
        return 1;
    }

    printf("received %zu bytes\n",response_len);


    parse_status_line(response);
    parse_headers(response);
    print_body(response);

    free(response);

    return 0;
}