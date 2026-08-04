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
#include <stdarg.h>

#define DEFAULT_URL "file:///tmp/browser_test.html"

typedef struct {
    char scheme[16];
    char host[256];
    char path[1024];
    int port;
    int view_source; // 0 for hide HTML tag, 1 for show full html source code
} URL;

typedef struct {
    int sockfd;
    char scheme[16];
    char host[256];
    int port;

    SSL_CTX *ctx;
    SSL *ssl;
} Connection;

static Connection cached_connection = {
    .sockfd=-1,
    .ctx=NULL,
    .ssl=NULL
};

//prototype
int connect_to_host(const char *host, int port);

//check url's scheme,host,port whether same as input url
int same_server(const URL *url) {
    return cached_connection.sockfd!=-1 &&
        strcmp(cached_connection.scheme,url->scheme)==0 &&
        strcmp(cached_connection.host,url->host)==0 &&
        cached_connection.port==url->port;
}

char *copy_string(const char *s) {
    size_t len = strlen(s);

    char *copy=malloc(len+1);
    if (copy==NULL){
        perror("malloc failed");
        return NULL;
    }

    memcpy(copy,s,len);
    copy[len] = '\0';
    return copy;
}

void close_cached_connection(void) {
    if (cached_connection.ssl != NULL) {
        SSL_shutdown(cached_connection.ssl);
        SSL_free(cached_connection.ssl);
        cached_connection.ssl = NULL;
    }

    if (cached_connection.ctx != NULL) {
        SSL_CTX_free(cached_connection.ctx);
        cached_connection.ctx = NULL;
    }

    if (cached_connection.sockfd != -1) {
        close(cached_connection.sockfd);
        cached_connection.sockfd = -1;
    }

    cached_connection.scheme[0] = '\0';
    cached_connection.host[0] = '\0';
    cached_connection.port = 0;
}

Connection *get_connection(URL *url){
    if (same_server(url)) {
        return &cached_connection;
    }

    close_cached_connection();

    int sockfd = connect_to_host(url->host,url->port);

    if (sockfd==-1){
        fprintf(stderr,"connect failed\n");
        return NULL;
    }

    cached_connection.sockfd = sockfd;
    cached_connection.port = url->port;

    strcpy(cached_connection.scheme,url->scheme);
    strcpy(cached_connection.host,url->host);

    //https reconnect needs use new ssl
    if (strcmp(url->scheme,"https")==0) {
        cached_connection.ctx = create_ssl_context();
        
        if (cached_connection.ctx==NULL) {
            close_cached_connection();
            return NULL;
        }

        cached_connection.ssl=connect_tls(sockfd,cached_connection.ctx,url->host);

        if (cached_connection.ssl==NULL) {
            close_cached_connection();
            return NULL;
        }
    }

    return &cached_connection;
}

int connection_send(Connection *conn,const char *data,size_t len) {
    if (conn->ssl != NULL) {
        return send_all_ssl(conn->ssl,data,len);
    }

    return send_all(conn->sockfd,data,len);
}

int connection_read(Connection *conn,char *buf,size_t len) {
    if (conn->ssl != NULL) {
        int n=SSL_read(conn->ssl,buf,(int)len);
        return n > 0 ? n : -1;
    }

    ssize_t n=recv(conn->sockfd,buf,len,0);
    return n > 0 ? (int)n : -1;

}

long get_content_length(const char *headers) {
    //get first header line，like HTTP/1.1 200 OK
    const char *line = strstr(headers,"\r\n"); 
    

    if (line==NULL){
        return -1;
    }

    line+=2; //skip \r\n

    while (*line !='\0') {
        //get next end of header line,it's Content-Length line
        const char *line_end = strstr(line,"\r\n"); 

        if (line_end == NULL || line_end==line){
            break;
        }

        const char *colon = memchr(line,':',(size_t)(line_end-line));

        if (colon!=NULL) {
            size_t name_len= (size_t)(colon-line);

            //check this header line is Content-Length
            if (name_len==strlen("Content-length") && strncasecmp(line,"Content-Length",name_len)==0) {
                const char *value = colon+1; //skip ':' to get length value

                // if have space or tab,skip it
                while(*value==' ' || *value=='\t') {
                    value++;
                }

                char *end;
                long length = strtol(value,&end,10);

                if (end==value || length < 0) { //get length value failed
                    return -1;
                }

                return length;
            }
        }

        line = line_end+2; //skip \r\n
    }

    return -1; //not found Content-Length
}

int parse_url(URL *u,const char *url){

    u->view_source = 0; //default not view html source code

    if (strncmp(url,"view-source:",12)==0){
        u->view_source = 1;
        url += 12; //skip "view-source:" parse left url
    }

    //data scheme
    //data:text/html,<h1>hello</h1>
    if (strncmp(url,"data:",5)==0){
        const char *comma = strchr(url,',');

        if (comma==NULL){
            fprintf(stderr,"data URL must contain a comma\n");
            return -1;
        }

        strcpy(u->scheme,"data");
        u->host[0]='\0';
        u->port=0;

        const char *content=comma+1; //skip comma for html content

        if (strlen(content)>=sizeof(u->path)) {
            fprintf(stderr, "data URL content too long\n");
            return -1;
        }

        strcpy(u->path,content);
        return 0;
    }

    const char *scheme_end = strstr(url,"://");

    if (scheme_end == NULL) {
        fprintf(stderr,"URL must contain ://\n");
        return -1;
    }

    size_t scheme_len = (size_t)(scheme_end - url);

    if (scheme_len>=sizeof(u->scheme)) {
        fprintf(stderr,"scheme too long\n");
        return -1;    
    }

    memcpy(u->scheme,url,scheme_len);
    u->scheme[scheme_len] = '\0';

    const char *rest = scheme_end + 3; //skip ://

    /*
        file:///path/to/index.html
        rest = "/path/to/index.html"
    */

    if (strcmp(u->scheme,"file")==0){
        if (rest[0]!='/') {
            fprintf(stderr,"file URL must use an absolute path\n");
            return -1;
        }

        u->host[0]='\0';
        u->port=0;

        if (strlen(rest)>=sizeof(u->path)) {
            fprintf(stderr,"path too long\n");
            return -1;
        }

        strcpy(u->path,rest);
        return 0;
    }


    if (strcmp(u->scheme,"http")==0){
        u->port = 80;
    } else if (strcmp(u->scheme,"https")==0){
        u->port = 443;
    }else{
        u->port=0;
    }

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
    char *colon = strchr(u->host,':');
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

char *read_file(const char *path) {
    FILE *fp=fopen(path,"rb");

    if (fp==NULL){
        perror(path);
        return NULL;
    }

    if (fseek(fp,0,SEEK_END)!=0){
        perror("fseek error");
        fclose(fp);
        return NULL;
    }

    long file_size=ftell(fp);

    if (file_size<0){
        perror("ftell error");
        fclose(fp);
        return NULL;
    }

    rewind(fp);

    char *content = malloc((size_t)file_size+1);

    if (content==NULL){
        perror("malloc failed");
        fclose(fp);
        return NULL;
    }

    size_t bytes_reads=fread(content,1,(size_t)file_size,fp);

    if (bytes_reads!=(size_t)file_size){
        fprintf(stderr,"could not read complete file\n");
        free(content);
        fclose(fp);
        return NULL;
    }

    content[bytes_reads] = '\0';
    fclose(fp);

    return content;
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

        if (err==SSL_ERROR_ZERO_RETURN) {
            break;
        }

        if ( err == SSL_ERROR_SYSCALL && n == 0) {
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

int append_request_part(char *buf,size_t cap,size_t *len ,const char *fmt,...) {
    if (*len>=cap){
        return -1;
    }

    va_list args;
    va_start(args,fmt);
    int n=vsnprintf(buf+*len,cap-*len,fmt,args);
    va_end(args);

    if (n<0 || (size_t)n>=cap-*len){
        return -1;
    }

    *len+=(size_t)n;
    return 0;
}

int add_header(char *buf,size_t cap,size_t *len,const char *name,const char *value) {
    return append_request_part(buf,cap,len,"%s: %s\r\n",name,value);
}

int finish_headers(char *buf,size_t cap,size_t *len) {
    return append_request_part(buf,cap,len,"\r\n");
}

int build_http_request(URL *url,char *buf,size_t cap,size_t *out_len) {
    size_t len=0;

    if (append_request_part(buf,cap,&len,"GET %s HTTP/1.1\r\n",url->path)!=0){
        return -1;
    }

    if (add_header(buf,cap,&len,"Host",url->host)!=0) {
        return -1;
    }

    if (add_header(buf,cap,&len,"Connection","keep-alive")!=0){
        return -1;
    }

    if (add_header(buf,cap,&len,"User-Agent","Tai-CI/1.0")!=0) {
        return -1;
    }

    if (finish_headers(buf,cap,&len)!=0) {
        return -1;
    }

    *out_len=len;
    return 0;
}

char *request(URL *url){

    //data scheme: return simple html content
    if (strcmp(url->scheme,"data")==0){
        return copy_string(url->path);
    }

    //file scheme: read url path
    if (strcmp(url->scheme,"file")==0){
        return read_file(url->path);
    }

    // http and https scheme: connect to server host
    int sockfd = connect_to_host(url->host,url->port);
    if (sockfd==-1){
        fprintf(stderr,"connect failed\n");
        return NULL;
    }

    char request_buf[2048];
    size_t request_len=0;

    if (build_http_request(url,request_buf,sizeof(request_buf),&request_len)!=0) {
        fprintf(stderr,"request too long\n");
        close(sockfd);
        return NULL;
    }


    char *response=NULL;
    size_t response_len = 0;
    
    if (strcmp(url->scheme,"https")==0){
        SSL_CTX *ctx = create_ssl_context();

        if (ctx==NULL){
            close(sockfd);
            return NULL;
        }

        SSL *ssl = connect_tls(sockfd,ctx,url->host);

        if (ssl==NULL){
            SSL_CTX_free(ctx);
            close(sockfd);
            return NULL;
        }

        if (send_all_ssl(ssl,request_buf,request_len)!=0) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
            SSL_CTX_free(ctx);
            close(sockfd);
            return NULL;
        }

        response = read_response_ssl(ssl,&response_len);
        
        SSL_shutdown(ssl);
        SSL_free(ssl);
        SSL_CTX_free(ctx);
        close(sockfd);
    } else{
        if (send_all(sockfd,request_buf,request_len)!=0){
            close(sockfd);
            return NULL;
        }

        response = read_response(sockfd,&response_len);

        close(sockfd);
    }


    if (response==NULL){
        return NULL;
    }


    parse_headers(response);
    
    char *content = copy_body(response);

    free(response);

    return content;
}

void show(const char *body){
    int in_tag=0;

    for(size_t i=0;body[i]!='\0';){
        
        //url have &lt; and &gt;  render as < and > for text not html label
        if (!in_tag && strncmp(body+i,"&lt;",4)==0) {
            putchar('<');
            i+=4;
            continue;
        }

        if (!in_tag && strncmp(body+i,"&gt;",4)==0){
            putchar('>');
            i+=4;
            continue;
        }

        char c=body[i];
        i++;

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

    if (url->view_source) { //show html source code
        fputs(body,stdout);
    } else{ //show rendered html
        show(body);
    }

    free(body);

    return 0;
}

int main(int argc, char **argv) {


    const char *input_url=NULL;

    if (argc==1){
        input_url=DEFAULT_URL;
    } else if (argc==2) {
        input_url=argv[1];
    } else {
        fprintf(stderr,"usage: %s [url]\n",argv[0]);
        return 1;
    }

    URL url;
    if (parse_url(&url,input_url) !=0){
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