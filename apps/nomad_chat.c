#define _POSIX_C_SOURCE 200809L
#include "reticulum/destination.h"
#include "reticulum/identity.h"
#include "reticulum/lxmf.h"
#include "reticulum/lxmf_delivery.h"
#include "reticulum/lxmf_store.h"
#include "reticulum/packet.h"
#include "reticulum/udp.h"
#include "reticulum/hal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EXIT_USAGE 64
#define EXIT_DATA 65
#define EXIT_UNAVAILABLE 69
#define CLI_MAX_FILE (LXMF_STORE_MAX_CONTENT + 1024u)

static void usage(FILE *f){fprintf(f,
"usage:\n"
"  nomad-chat init IDENTITY\n"
"  nomad-chat address IDENTITY\n"
"  nomad-chat pack IDENTITY DESTINATION TEXT OUTPUT\n"
"  nomad-chat unpack FILE\n"
"  nomad-chat send-udp IDENTITY DESTINATION DEST_PUBLIC HOST PORT TEXT\n"
"  nomad-chat receive-udp IDENTITY SOURCE_PUBLIC PORT [TIMEOUT_MS]\n"
"  nomad-chat send-file FILE\n"
"  nomad-chat history STORE\n"
"  nomad-chat repl IDENTITY DESTINATION STORE\n");}
static void hex_print(FILE *f,const uint8_t *p,size_t n){for(size_t i=0;i<n;i++)fprintf(f,"%02x",p[i]);}
static int hex_parse(const char *s,uint8_t *out,size_t n){if(strlen(s)!=2*n)return 0;for(size_t i=0;i<n;i++){char b[3]={s[2*i],s[2*i+1],0};char *end;long v=strtol(b,&end,16);if(*end||v<0||v>255)return 0;out[i]=(uint8_t)v;}return 1;}
static int load_identity(const char *path,rns_identity *id){uint8_t key[64];FILE *f=fopen(path,"rb");if(!f){fprintf(stderr,"cannot open identity %s: %s\n",path,strerror(errno));return 0;}size_t n=fread(key,1,sizeof key,f);int extra=fgetc(f);fclose(f);if(n!=sizeof key||extra!=EOF||!rns_identity_from_private(id,key)){fprintf(stderr,"invalid identity file %s\n",path);return 0;}return 1;}
static int save_identity(const char *path,const rns_identity *id){size_t n=strlen(path);if(n>1000)return 0;char tmp[1024];snprintf(tmp,sizeof tmp,"%s.tmp",path);uint8_t key[64];if(!rns_identity_export_private(id,key))return 0;int fd=open(tmp,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)return 0;size_t off=0;while(off<sizeof key){ssize_t w=write(fd,key+off,sizeof key-off);if(w<=0){close(fd);unlink(tmp);return 0;}off+=(size_t)w;}int ok=fsync(fd)==0&&close(fd)==0&&rename(tmp,path)==0;if(!ok)unlink(tmp);return ok;}
static int delivery_hash(const rns_identity *id,uint8_t out[16]){const char *aspects[]={"delivery"};return rns_destination_hash(id,"lxmf",aspects,1,out);}
static int read_file(const char *path,uint8_t *buf,size_t cap,size_t *len){FILE *f=fopen(path,"rb");if(!f)return 0;*len=fread(buf,1,cap,f);int extra=fgetc(f);int ok=!ferror(f)&&extra==EOF;fclose(f);return ok;}
static int write_file(const char *path,const uint8_t *p,size_t n){FILE *f=fopen(path,"wb");if(!f)return 0;int ok=fwrite(p,1,n,f)==n&&fflush(f)==0;ok=ok&&fsync(fileno(f))==0;ok=ok&&fclose(f)==0;return ok;}
static int build_message(const rns_identity *id,const uint8_t dest[16],const char *text,uint8_t *packed,size_t cap,size_t *n,lxmf_message_t *decoded){lxmf_message_t m={0};memcpy(m.destination,dest,16);if(!delivery_hash(id,m.source))return 0;m.timestamp=(double)time(NULL);m.content.data=(const uint8_t *)text;m.content.len=strlen(text);if(m.content.len>LXMF_STORE_MAX_CONTENT)return 0;if(lxmf_pack(&m,lxmf_identity_signer,(void *)id,packed,cap,n)!=LXMF_OK)return 0;return lxmf_unpack(packed,*n,NULL,NULL,decoded)==LXMF_OK;}
static int cmd_init(const char *path){if(access(path,F_OK)==0){fprintf(stderr,"identity already exists: %s\n",path);return EXIT_DATA;}rns_identity id;if(!rns_identity_generate(&id)||!save_identity(path,&id)){fprintf(stderr,"failed to create identity: %s\n",strerror(errno));return EXIT_DATA;}uint8_t hash[16];if(!delivery_hash(&id,hash))return EXIT_DATA;hex_print(stdout,hash,16);putchar('\n');return 0;}
static int cmd_address(const char *path){rns_identity id;uint8_t hash[16];if(!load_identity(path,&id)||!delivery_hash(&id,hash))return EXIT_DATA;hex_print(stdout,hash,16);putchar('\n');return 0;}
static int cmd_pack(const char *identity,const char *dest_s,const char *text,const char *output){rns_identity id;uint8_t dest[16],buf[CLI_MAX_FILE];size_t n;lxmf_message_t decoded;if(!load_identity(identity,&id)||!hex_parse(dest_s,dest,16)){fprintf(stderr,"invalid destination\n");return EXIT_DATA;}if(!build_message(&id,dest,text,buf,sizeof buf,&n,&decoded)||!write_file(output,buf,n)){fprintf(stderr,"could not pack message\n");return EXIT_DATA;}hex_print(stdout,decoded.message_id,32);putchar('\n');return 0;}
static int cmd_unpack(const char *path){uint8_t buf[CLI_MAX_FILE];size_t n;lxmf_message_t m;if(!read_file(path,buf,sizeof buf,&n)||lxmf_unpack(buf,n,NULL,NULL,&m)!=LXMF_OK){fprintf(stderr,"invalid LXMF file\n");return EXIT_DATA;}printf("message_id: ");hex_print(stdout,m.message_id,32);printf("\ndestination: ");hex_print(stdout,m.destination,16);printf("\nsource: ");hex_print(stdout,m.source,16);printf("\ntimestamp: %.6f\nstatus: signature-unverified\ntitle: ",m.timestamp);if(m.title.len)fwrite(m.title.data,1,m.title.len,stdout);printf("\ncontent: ");if(m.content.len)fwrite(m.content.data,1,m.content.len,stdout);putchar('\n');return 0;}
static bool history_line(void *ctx,const lxmf_store_message_t *m){(void)ctx;hex_print(stdout,m->message_id,32);printf(" %u %.6f ",(unsigned)m->status,m->timestamp);fwrite(m->content.data,1,m->content.len,stdout);putchar('\n');return true;}
static int cmd_history(const char *path){lxmf_store_t s={0};if(lxmf_store_open(&s,path)!=LXMF_OK){fprintf(stderr,"cannot open history\n");return EXIT_DATA;}lxmf_status_t st=lxmf_store_list(&s,history_line,NULL);lxmf_store_close(&s);return st==LXMF_OK?0:EXIT_DATA;}
static int cmd_repl(const char *identity_path,const char *dest_s,const char *store_path){rns_identity id;uint8_t dest[16];if(!load_identity(identity_path,&id)||!hex_parse(dest_s,dest,16))return EXIT_DATA;lxmf_store_t store={0};if(lxmf_store_open(&store,store_path)!=LXMF_OK)return EXIT_DATA;char line[LXMF_STORE_MAX_CONTENT+2];puts("local outbox; /history lists messages, /quit exits");while(fputs("> ",stdout),fflush(stdout),fgets(line,sizeof line,stdin)){size_t len=strlen(line);if(len&&line[len-1]=='\n')line[--len]=0;else if(!feof(stdin)){int c;while((c=getchar())!='\n'&&c!=EOF){}fprintf(stderr,"line too long\n");continue;}if(!strcmp(line,"/quit"))break;if(!strcmp(line,"/history")){lxmf_store_list(&store,history_line,NULL);continue;}if(!len)continue;uint8_t packed[CLI_MAX_FILE];size_t n;lxmf_message_t decoded;if(!build_message(&id,dest,line,packed,sizeof packed,&n,&decoded)){fprintf(stderr,"pack failed\n");continue;}lxmf_store_message_t item={0};memcpy(item.message_id,decoded.message_id,32);memcpy(item.destination,decoded.destination,16);memcpy(item.source,decoded.source,16);item.timestamp=decoded.timestamp;item.status=LXMF_DELIVERY_QUEUED;item.content=decoded.content;bool inserted;if(lxmf_store_put(&store,&item,&inserted)!=LXMF_OK)fprintf(stderr,"outbox write failed\n");else{printf("queued ");hex_print(stdout,item.message_id,32);putchar('\n');}}lxmf_store_close(&store);return 0;}

static int parse_port(const char *text,uint16_t *port){char *end=NULL;unsigned long value=strtoul(text,&end,10);if(!text[0]||!end||*end||value==0||value>65535)return 0;*port=(uint16_t)value;return 1;}
static int public_identity(const char *hex,rns_identity *identity){uint8_t bytes[64];return hex_parse(hex,bytes,sizeof bytes)&&rns_identity_from_public(identity,bytes);}
static int cmd_send_udp(const char *identity_path,const char *dest_s,const char *public_s,const char *host,const char *port_s,const char *text){
    rns_identity source,destination;uint8_t expected[16],packet[RNS_MTU];size_t packet_length;uint16_t port;rns_udp_endpoint_t *udp=NULL;lxmf_message_t message={0};
    if(!load_identity(identity_path,&source)||!public_identity(public_s,&destination)||!hex_parse(dest_s,message.destination,16)||!parse_port(port_s,&port)){fprintf(stderr,"invalid identity, destination, public key, or port\n");return EXIT_DATA;}
    if(!delivery_hash(&destination,expected)||memcmp(expected,message.destination,16)!=0){fprintf(stderr,"destination does not match public identity\n");return EXIT_DATA;}
    if(!delivery_hash(&source,message.source)){return EXIT_DATA;}message.timestamp=(double)time(NULL);message.content.data=(const uint8_t *)text;message.content.len=strlen(text);
    if(lxmf_opportunistic_packet_pack(&message,&source,&destination,packet,sizeof packet,&packet_length)!=LXMF_OK){fprintf(stderr,"message is too large for opportunistic delivery\n");return EXIT_DATA;}
    if(rns_udp_endpoint_create(&udp,RNS_UDP_IPV4)!=RNS_OK||rns_udp_connect(udp,host,port)!=RNS_OK||rns_udp_send(udp,packet,packet_length)!=RNS_OK){fprintf(stderr,"UDP delivery failed\n");rns_udp_endpoint_destroy(udp);return EXIT_UNAVAILABLE;}
    rns_udp_endpoint_destroy(udp);puts("sent");return 0;
}
typedef struct {const rns_identity *local;const rns_identity *source;uint8_t source_hash[16];int received;} receive_context;
static const rns_identity *resolve_source(void *ctx,const uint8_t hash[16]){receive_context *r=ctx;return memcmp(hash,r->source_hash,16)==0?r->source:NULL;}
static rns_status_t receive_udp_packet(const uint8_t *packet,size_t packet_length,const rns_udp_address_t *address,void *ctx){receive_context *r=ctx;lxmf_identity_verifier_context_t verifier={resolve_source,r};lxmf_message_t message;uint8_t plaintext[RNS_MTU];size_t plaintext_length;(void)address;lxmf_status_t status=lxmf_opportunistic_packet_unpack(packet,packet_length,r->local,lxmf_identity_verifier,&verifier,plaintext,sizeof plaintext,&plaintext_length,&message);if(status!=LXMF_OK)return RNS_ERROR_PROTOCOL;printf("from: ");hex_print(stdout,message.source,16);printf("\ncontent: ");fwrite(message.content.data,1,message.content.len,stdout);putchar('\n');r->received=1;return RNS_OK;}
static int cmd_receive_udp(const char *identity_path,const char *source_s,const char *port_s,const char *timeout_s){rns_identity local,source;uint16_t port;unsigned long timeout=30000;char *end=NULL;rns_udp_endpoint_t *udp=NULL;receive_context context={0};uint64_t started,now;if(!load_identity(identity_path,&local)||!public_identity(source_s,&source)||!parse_port(port_s,&port))return EXIT_DATA;if(timeout_s){timeout=strtoul(timeout_s,&end,10);if(!timeout_s[0]||!end||*end||timeout==0)return EXIT_DATA;}context.local=&local;context.source=&source;delivery_hash(&source,context.source_hash);if(rns_udp_endpoint_create(&udp,RNS_UDP_IPV4)!=RNS_OK||rns_udp_bind(udp,"0.0.0.0",port)!=RNS_OK){rns_udp_endpoint_destroy(udp);return EXIT_UNAVAILABLE;}if(rns_hal_monotonic_ms(&started)!=RNS_OK){rns_udp_endpoint_destroy(udp);return EXIT_UNAVAILABLE;}do{size_t received=0;rns_status_t status=rns_udp_poll(udp,1,receive_udp_packet,&context,&received);if(status!=RNS_OK){fprintf(stderr,"received invalid packet\n");rns_udp_endpoint_destroy(udp);return EXIT_DATA;}if(context.received)break;rns_hal_sleep_ms(10);rns_hal_monotonic_ms(&now);}while(now-started<timeout);rns_udp_endpoint_destroy(udp);if(!context.received){fprintf(stderr,"receive timed out\n");return EXIT_UNAVAILABLE;}return 0;}
int main(int argc,char **argv){if(argc<2){usage(stderr);return EXIT_USAGE;}if(!strcmp(argv[1],"init")&&argc==3)return cmd_init(argv[2]);if(!strcmp(argv[1],"address")&&argc==3)return cmd_address(argv[2]);if(!strcmp(argv[1],"pack")&&argc==6)return cmd_pack(argv[2],argv[3],argv[4],argv[5]);if(!strcmp(argv[1],"unpack")&&argc==3)return cmd_unpack(argv[2]);if(!strcmp(argv[1],"history")&&argc==3)return cmd_history(argv[2]);if(!strcmp(argv[1],"repl")&&argc==5)return cmd_repl(argv[2],argv[3],argv[4]);if(!strcmp(argv[1],"send-udp")&&argc==8)return cmd_send_udp(argv[2],argv[3],argv[4],argv[5],argv[6],argv[7]);if(!strcmp(argv[1],"receive-udp")&&(argc==5||argc==6))return cmd_receive_udp(argv[2],argv[3],argv[4],argc==6?argv[5]:NULL);if(!strcmp(argv[1],"send-file")&&argc==3){(void)argv;fprintf(stderr,"network delivery runtime is not implemented; file was not sent\n");return EXIT_UNAVAILABLE;}usage(stderr);return EXIT_USAGE;}
