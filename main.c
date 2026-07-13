#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <strings.h>
#include <netdb.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>


typedef struct {
    char scheme[16];
    char host[256];
    char path[1024];
    int port;
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

    assert(strcmp(u->scheme,"http")==0 || strcmp(u->scheme,"https")==0);

    if (strcmp(u->scheme,"http")==0){
        u->port = 80;
    } else{
        u->port = 443;
    }

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

    //support custom port
    char *colon = strchr(u->host,":");
    if (colon!=NULL){
        *colon = '\0';
        u->port = atoi(colon+1);
        
        if (u->port <=0 || u->port > 65535) {
            fprintf(stderr,"invalid port\n");
            return -1;
        }
    }

    return 0;
}


int connect_to_host(const char *host,int port) {
    struct addrinfo hints; //for one domain name for multiple IP address
    struct addrinfo *result;
    struct addrinfo *rp;
    int sockfd = -1;

    char port_str[16];
    snprintf(port_str,sizeof(port_str),"%d",port);


    memset(&hints,0,sizeof(hints));

    hints.ai_family = AF_INET; //ipv4
    hints.ai_socktype = SOCK_STREAM; // TCP stream
    hints.ai_protocol = IPPROTO_TCP; // TCP

    int err = getaddrinfo(host,port_str,&hints,&result);
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

SSL_CTX *create_ssl_context(void) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());

    if (ctx==NULL) {
        ERR_print_errors_fp(stderr);
        return NULL;
    }

    if (SSL_CTX_set_default_verify_paths(ctx)!=1) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ctx);
        return NULL;
    }

    return ctx;
}

SSL *connect_tls(int sockfd,SSL_CTX *ctx,const char *host){
    SSL *ssl = SSL_new(ctx);

    if (ssl==NULL){
        ERR_print_errors_fp(stderr);
        return NULL;
    }

    //SNI: tell server which hostname want to connect
    //many https server one IP have many website，if no SNI maybe take wrong certificate
    if (SSL_set_tlsext_host_name(ssl,host)!=1){
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        return NULL;
    }

    // hostname verification: check certificate is really belong this host
    if (SSL_set1_host(ssl,host)!=1){
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        return NULL;
    }

    SSL_set_verify(ssl,SSL_VERIFY_PEER,NULL);

    if (SSL_set_fd(ssl,sockfd)!=1){
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        return NULL;
    }

    if (SSL_connect(ssl)!=1){
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        return NULL;
    }

    return ssl;
}

//for https send data
int send_all_ssl(SSL *ssl,const char *data,size_t len) {
    size_t sent_total = 0;

    while (sent_total < len) {
        int sent = SSL_write(ssl,data+sent_total,(int)(len-sent_total));
        
        if (sent <= 0) {
            ERR_print_errors_fp(stderr);
            return -1;
        }

        sent_total +=(size_t)sent;

    }

    return 0;
}

char *read_response_ssl(SSL *ssl,size_t *out_len) {
    size_t cap = 4096;
    size_t len = 0;

    char *response = malloc(cap+1);
    if (response==NULL){
        perror("malloc error");
        return NULL;
    }

    while (1){
        if (len==cap) {
            size_t new_cap=cap*2;

            char *new_response = realloc(response,new_cap+1);
            if (new_response==NULL) {
                perror("realloc error");
                free(response);
                return NULL;
            }

            response=new_response;
            cap = new_cap;
        }

        int n=SSL_read(ssl,response+len,(int)(cap-len));
        
        if (n>0) {
            len+=(size_t)n;
            continue;
        }

        //If SSL read fail
        int err=SSL_get_error(ssl,n);

        if (err==SSL_ERROR_ZERO_RETURN || err == SSL_ERROR_SYSCALL && n == 0) {
            break;
        }

        fprintf(stderr,"SSL_read failed\n");
        ERR_print_errors_fp(stderr);
        free(response);
        return NULL;
    }

    response[len]='\0';
    *out_len=len;

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

    // char *version = line;
    
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

    // char *explanation = space2+1;

    // fprintf(stderr, "version     = %s\n", version);
    // fprintf(stderr, "status      = %s\n", status);
    // fprintf(stderr, "explanation = %s\n", explanation);

}

void parse_headers(char *response) {
    char *status_end = strstr(response,"\r\n");
    char *headers_end = strstr(response,"\r\n\r\n");

    if (status_end == NULL || headers_end == NULL) {
        fprintf(stderr,"invalid response headers\n");
        return;
    }

    char *line = status_end + 2; //skip \r\n
    
    // fprintf(stderr,"---- headers ----\n");

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

            // fprintf(stderr,"%s = %s\n",header,value);

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

}

char* copy_body(const char *response) {
    const char *body = strstr(response,"\r\n\r\n");
    
    if (body==NULL){
        fprintf(stderr,"invalid response: no body\n");
        return NULL;
    }

    body += 4; //skip \r\n\r\n

    size_t body_len = strlen(body);

    char *content = malloc(body_len+1);
    if (content==NULL){
        perror("malloc error");
        return NULL;
    }

    memcpy(content,body,body_len);
    content[body_len] = '\0';

    return content;
}

char *request(URL *url){
    int sockfd = connect_to_host(url->host);
    if (sockfd==-1){
        fprintf(stderr,"connect failed\n");
        return NULL;
    }

    // fprintf(stderr,"connect to %s:80\n",url->host);

    char request[2048];
    
    int n=snprintf(
        request,
        sizeof(request),
        "GET %s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "\r\n",
        url->path,
        url->host
    );

    if (n<0 || n>=(int)sizeof(request)) {
        fprintf(stderr,"request too long\n");
        close(sockfd);
        return NULL;
    }

    // fprintf(stderr,"---- request ----\n");
    // fprintf(stderr,"%s\n",request);
    // fprintf(stderr,"---- end request ----\n");


    if (send_all(sockfd,request,strlen(request))!=0){
        close(sockfd);
        return NULL;
    }

    size_t response_len=0;
    // fprintf(stderr,"request sent: %zu bytes\n",strlen(request));
    char *response = read_response(sockfd,&response_len);

    close(sockfd);

    if (response==NULL){
        return NULL;
    }

    // fprintf(stderr,"received %zu bytes\n",response_len);


    // parse_status_line(response);
    parse_headers(response);
    
    char *content = copy_body(response);

    free(response);

    return content;
}

void show(const char *body){
    int in_tag=0;

    for(size_t i=0;body[i]!='\0';i++){
        char c=body[i];

        if (c=='<'){
            in_tag=1;
        } else if (c=='>'){
            in_tag=0;
        }else if (!in_tag){
            putchar(c);
        }
    }
}

int load(URL *url) {
    char *body = request(url);

    if (body==NULL) {
        return -1;
    }

    show(body);

    free(body);

    return 0;
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

    // fprintf(stderr,"scheme = %s\n",url.scheme);
    // fprintf(stderr,"host   = %s\n", url.host);
    // fprintf(stderr,"path   = %s\n", url.path);

    
    if (load(&url)!=0) {
        return 1;
    }


    return 0;
}