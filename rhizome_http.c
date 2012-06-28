/*
Serval Distributed Numbering Architecture (DNA)
Copyright (C) 2010 Paul Gardner-Stephen
 
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
 
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
 
You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <sys/types.h>
#include <sys/socket.h>
#include <signal.h>
#include <ctype.h>

#include "serval.h"
#include "rhizome.h"

/*
  HTTP server and client code for rhizome transfers.

 */

int rhizome_server_socket=-1;

rhizome_http_request *rhizome_live_http_requests[RHIZOME_SERVER_MAX_LIVE_REQUESTS];
int rhizome_server_live_request_count=0;

// Format icon data using:
//   od -vt u1 ~/Downloads/favicon.ico | cut -c9- | sed 's/  */,/g'
unsigned char favicon_bytes[]={
0,0,1,0,1,0,16,16,16,0,0,0,0,0,40,1
,0,0,22,0,0,0,40,0,0,0,16,0,0,0,32,0
,0,0,1,0,4,0,0,0,0,0,128,0,0,0,0,0
,0,0,0,0,0,0,16,0,0,0,0,0,0,0,104,158
,168,0,163,233,247,0,104,161,118,0,0,0,0,0,0,0
,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
,0,0,0,0,0,0,0,0,0,0,0,0,0,0,17,17
,17,17,17,18,34,17,17,18,34,17,17,18,34,17,17,2
,34,17,17,18,34,17,16,18,34,1,17,17,1,17,1,17
,1,16,1,16,17,17,17,17,1,17,16,16,17,17,17,17
,1,17,18,34,17,17,17,16,17,17,2,34,17,17,17,16
,17,16,18,34,17,17,17,16,17,1,17,1,17,17,17,18
,34,17,17,16,17,17,17,18,34,17,17,18,34,17,17,18
,34,17,17,18,34,17,17,16,17,17,17,18,34,17,17,16
,17,17,17,17,17,0,17,1,17,17,17,17,17,17,0,0
,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
int favicon_len=318;

int rhizome_server_start()
{
  if (rhizome_server_socket>-1) return 0;

  /* Only try to start http server periodically */
  if (rhizome_server_socket<-1) { rhizome_server_socket++; return -1; }

  struct sockaddr_in address;
  int on=1;

  if (debug&DEBUG_RHIZOME) WHYF("Trying to start rhizome server.");

  rhizome_server_socket=socket(AF_INET,SOCK_STREAM,0);
  if (rhizome_server_socket<0)
    {
      rhizome_server_socket=-1000;
      return WHY("socket() failed starting rhizome http server");
    }

  setsockopt(rhizome_server_socket, SOL_SOCKET,  SO_REUSEADDR,
                  (char *)&on, sizeof(on));

  bzero((char *) &address, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(RHIZOME_HTTP_PORT);
  if (bind(rhizome_server_socket, (struct sockaddr *) &address,
	   sizeof(address)) < 0) 
    {
      fd_teardown(rhizome_server_socket);
      rhizome_server_socket=-1000;
      if (debug&DEBUG_RHIZOME)  WHY("bind() failed starting rhizome http server");
      return -1;
    }

  int rc = ioctl(rhizome_server_socket, FIONBIO, (char *)&on);
  if (rc < 0)
  {
    perror("ioctl() failed");
    fd_teardown(rhizome_server_socket);
    rhizome_server_socket=-1;
    exit(-1);
  }

  if (listen(rhizome_server_socket,20))
    {
      fd_teardown(rhizome_server_socket);
      rhizome_server_socket=-1;
      return WHY("listen() failed starting rhizome http server");
    }

  /* Add Rhizome HTTPd server to list of file descriptors to watch */
  fd_watch(rhizome_server_socket,rhizome_server_poll,POLLIN);

  return 0;
}

void rhizome_client_poll(int fd)
{
  int rn;

  for(rn=0;rn<rhizome_server_live_request_count;rn++)
    {
      rhizome_http_request *r=rhizome_live_http_requests[rn];
      if (r->socket!=fd) continue;
      switch(r->request_type) 
	{
	case RHIZOME_HTTP_REQUEST_RECEIVING:
	  /* Keep reading until we have two CR/LFs in a row */
	  r->request[r->request_length]=0;
	  
	  sigPipeFlag=0;
	  
	  errno=0;
	  int bytes=read(r->socket,&r->request[r->request_length],
			 RHIZOME_HTTP_REQUEST_MAXLEN-r->request_length-1);
	  
	  /* If we got some data, see if we have found the end of the HTTP request */
	  if (bytes>0) {
	    int i=r->request_length-160;
	    int lfcount=0;
	    if (i<0) i=0;
	    r->request_length+=bytes;
	    if (r->request_length<RHIZOME_HTTP_REQUEST_MAXLEN)
	      r->request[r->request_length]=0;
	    if (0)
	      dump("request",(unsigned char *)r->request,r->request_length);
	    for(;i<(r->request_length+bytes);i++)
	      {
		switch(r->request[i]) {
		case '\n': lfcount++; break;
		case '\r': /* ignore CR */ break;
		case 0: /* ignore NUL (telnet inserts them) */ break;
		default: lfcount=0; break;
		}
		if (lfcount==2) break;
	      }
	    if (lfcount==2) {
	      /* We have the request. Now parse it to see if we can respond to it */
	      rhizome_server_parse_http_request(rn,r);
	    }
	    
	    r->request_length+=bytes;
	  } 

	  if (sigPipeFlag||((bytes==0)&&(errno==0))) {
	    /* broken pipe, so close connection */
	    WHY("Closing connection due to sigpipe");
	    rhizome_server_close_http_request(rn);
	    continue;
	  }	 
	  break;
	default:
	  /* Socket already has request -- so just try to send some data. */
	  rhizome_server_http_send_bytes(rn,r);
	  break;
      }
      /* We have processed the connection that has activity, so we can return
         immediately */
      return;
    }

  
  return;
}


void rhizome_server_poll(int ignored_file_descriptor)
{
  struct sockaddr addr;
  unsigned int addr_len=0;
  int sock;

  /* Deal with any new requests */

  while ((rhizome_server_live_request_count<RHIZOME_SERVER_MAX_LIVE_REQUESTS)
	 &&((sock=accept(rhizome_server_socket,&addr,&addr_len))>-1))
    {
      rhizome_http_request *request = calloc(sizeof(rhizome_http_request),1);	
      request->socket=sock;
      /* We are now trying to read the HTTP request */
      request->request_type=RHIZOME_HTTP_REQUEST_RECEIVING;
      rhizome_live_http_requests[rhizome_server_live_request_count++]=request;	   
      /* Watch for input */
      fd_watch(request->socket,rhizome_client_poll,POLLIN);
    }

}

int rhizome_server_close_http_request(int i)
{
  fd_teardown(rhizome_live_http_requests[i]->socket);

  rhizome_server_free_http_request(rhizome_live_http_requests[i]);
  /* Make it null, so that if we are the list in the list, the following
     assignment still yields the correct behaviour */
  rhizome_live_http_requests[i]=NULL;
  rhizome_live_http_requests[i]=
    rhizome_live_http_requests[rhizome_server_live_request_count-1];
  rhizome_server_live_request_count--;
  return 0;
}

int rhizome_server_free_http_request(rhizome_http_request *r)
{
  if (r->buffer&&r->buffer_size) free(r->buffer);
  if (r->blob) sqlite3_blob_close(r->blob);
  free(r);
  return 0;
}

void hexFilter(char *s)
{
  char *t;
  for (t = s; *s; ++s)
    if (isxdigit(*s))
      *t++ = *s;
  *t = '\0';
}

int rhizome_server_sql_query_http_response(int rn,rhizome_http_request *r,
					   char *column,char *table,char *query_body,
					   int bytes_per_row,int dehexP)
{
  /* Run the provided SQL query progressively and return the values of the first
     column it returns.  As the result list may be very long, we will add the
     LIMIT <skip>,<count> clause to do it piece by piece.

     Otherwise, the response is prefixed by a 256 byte header, including the public
     key of the sending node, and allowing space for information about encryption of
     the body, although encryption is not yet implemented here.
 */

  if (r->buffer) { free(r->buffer); r->buffer=NULL; }
  r->buffer_size=16384;
  r->buffer=malloc(r->buffer_size);
  if (!r->buffer) return WHY("malloc() failed to allocate response buffer");
  r->buffer_length=0;
  r->buffer_offset=0;
  r->source_record_size=bytes_per_row;
  r->source_count = 0;
  sqlite_exec_int64(&r->source_count, "SELECT COUNT(*) %s", query_body);

  /* Work out total response length */
  long long response_bytes=256+r->source_count*r->source_record_size;
  rhizome_server_http_response_header(r,200,"servalproject.org/rhizome-list", 
				      response_bytes);
  WHYF("headers consumed %d bytes.",r->buffer_length);

  /* Clear and prepare response header */
  bzero(&r->buffer[r->buffer_length],256);
  
  r->buffer[r->buffer_length]=0x01; /* type of response (list) */
  r->buffer[r->buffer_length+1]=0x01; /* version of response */

  WHYF("Found %lld records.",r->source_count);
  /* Number of records we intend to return */
  r->buffer[r->buffer_length+4]=(r->source_count>>0)&0xff;
  r->buffer[r->buffer_length+5]=(r->source_count>>8)&0xff;
  r->buffer[r->buffer_length+6]=(r->source_count>>16)&0xff;
  r->buffer[r->buffer_length+7]=(r->source_count>>24)&0xff;

  r->buffer_length+=256;

  /* copy our public key in to bytes 32+ */
  WHY("no function yet exists to obtain our public key?");

  /* build templated query */
  strbuf b = strbuf_local(r->source, sizeof r->source);
  strbuf_sprintf(b, "SELECT %s,rowid %s", column, query_body);
  if (strbuf_overrun(b))
    WHYF("SQL query overrun: %s", strbuf_str(b));
  r->source_index=0;
  r->source_flags=dehexP;

  DEBUGF("buffer_length=%d",r->buffer_length);

  /* Populate spare space in buffer with rows of data */
  return rhizome_server_sql_query_fill_buffer(rn, r, table, column);
}

int rhizome_server_sql_query_fill_buffer(int rn,rhizome_http_request *r, char *table, char *column)
{
  unsigned char blob_value[r->source_record_size*2+1];

  WHYF("populating with sql rows at offset %d",r->buffer_length);
  if (r->source_index>=r->source_count)
    {
      /* All done */
      return 0;
    }

  int record_count=(r->buffer_size-r->buffer_length)/r->source_record_size;
  if (record_count<1) {
    WHYF("r->buffer_size=%d, r->buffer_length=%d, r->source_record_size=%d",
	   r->buffer_size, r->buffer_length, r->source_record_size);
    return WHY("Not enough space to fit any records");
  }

  char query[1024];
  snprintf(query,1024,"%s LIMIT %lld,%d",r->source,r->source_index,record_count);

  sqlite3_stmt *statement;
  WHY(query);
  switch (sqlite3_prepare_v2(rhizome_db,query,-1,&statement,NULL))
    {
    case SQLITE_OK: case SQLITE_DONE: case SQLITE_ROW:
      break;
    default:
      sqlite3_finalize(statement);
      sqlite3_close(rhizome_db);
      rhizome_db=NULL;
      WHY(query);
      WHY(sqlite3_errmsg(rhizome_db));
      return WHY("Could not prepare sql statement.");
    }
  while(((r->buffer_length+r->source_record_size)<r->buffer_size)
	&&(sqlite3_step(statement)==SQLITE_ROW))
    {
      r->source_index++;
      
      if (sqlite3_column_count(statement)!=2) {
	sqlite3_finalize(statement);
	return WHY("sqlite3 returned multiple columns for a single column query");
      }
      sqlite3_blob *blob;
      const unsigned char *value;
      int column_type=sqlite3_column_type(statement, 0);
      switch(column_type) {
      case SQLITE_TEXT:	value=sqlite3_column_text(statement, 0); break;
      case SQLITE_BLOB:
	WHYF("table='%s',col='%s',rowid=%lld",
	       table, column,
	       sqlite3_column_int64(statement,1));
	if (sqlite3_blob_open(rhizome_db,"main",table,column,
			      sqlite3_column_int64(statement,1) /* rowid */,
			      0 /* read only */,&blob)!=SQLITE_OK)
	  {
	    WHY("Couldn't open blob");
	    continue;
	  }
	if (sqlite3_blob_read(blob,&blob_value[0],
			  /* copy number of bytes based on whether we need to
			     de-hex the string or not */
			      r->source_record_size*(1+(r->source_flags&1)),0)
	    !=SQLITE_OK) {
	  WHY("Couldn't read from blob");
	  sqlite3_blob_close(blob);
	  continue;
	}
	WHY("Did read blob");
	value=blob_value;
	sqlite3_blob_close(blob);
	break;
      default:
	/* improper column type, so don't include in report */
	WHY("Bad column type");
	WHYF("colunnt_type=%d",column_type);
	continue;
      }
      if (r->source_flags&1) {
	/* hex string to be converted */
	int i;
	for(i=0;i<r->source_record_size;i++)
	  /* convert the two nybls and make a byte */
	  r->buffer[r->buffer_length+i]
	    =(hexvalue(value[i<<1])<<4)|hexvalue(value[(i<<1)+1]);
      } else
	/* direct binary value */
	bcopy(value,&r->buffer[r->buffer_length],r->source_record_size);
      r->buffer_length+=r->source_record_size;
      
    }
  sqlite3_finalize(statement);

  return 0;  
}


int rhizome_server_parse_http_request(int rn,rhizome_http_request *r)
{
  char id[1024];

  /* Switching to writing, so update the call-back */
  fd_watch(r->socket,rhizome_client_poll,POLLOUT);	
  
  /* Clear request type flags */
  r->request_type=0;

  if (strlen(r->request)<1024) {
    if (!strncasecmp(r->request,"GET /favicon.ico HTTP/1.",
		     strlen("GET /favicon.ico HTTP/1.")))
      {
	r->request_type=RHIZOME_HTTP_REQUEST_FAVICON;
	rhizome_server_http_response_header(r,200,"image/vnd.microsoft.icon",
					    favicon_len);	
      }
    else if (!strncasecmp(r->request,"GET /rhizome/groups HTTP/1.",
		     strlen("GET /rhizome/groups HTTP/1.")))
      {
	/* Return the list of known groups */
	WHYF("get /rhizome/groups (list of groups)");
	rhizome_server_sql_query_http_response(rn,r,"id","groups","from groups",32,1);
      }
    else if (!strncasecmp(r->request,"GET /rhizome/files HTTP/1.",
		     strlen("GET /rhizome/files HTTP/1.")))
      {
	/* Return the list of known files */
	WHYF("get /rhizome/files (list of files)");
	rhizome_server_sql_query_http_response(rn,r,"id","files","from files",32,1);
      }
    else if (!strncasecmp(r->request,"GET /rhizome/bars HTTP/1.",
		     strlen("GET /rhizome/bars HTTP/1.")))
      {
	/* Return the list of known files */
	WHYF("get /rhizome/bars (list of BARs)");
	rhizome_server_sql_query_http_response(rn,r,"bar","manifests","from manifests",32,0);
      }
    else if (sscanf(r->request,"GET /rhizome/file/%s HTTP/1.", id)==1)
      {
	/* Stream the specified file */

	int dud=0;
	int i;
	hexFilter(id);
	WHYF("get /rhizome/file/ [%s]",id);
	
	// Check for range: header, and return 206 if returning partial content
	for(i=0;i<strlen(id);i++) if (!isxdigit(id[i])) dud++;
	if (dud) rhizome_server_simple_http_response(r,400,"<html><h1>That doesn't look like hex to me.</h1></html>\r\n");
	else {
	  str_toupper_inplace(id);
	  long long rowid = -1;
	  sqlite_exec_int64(&rowid, "select rowid from files where id='%s';", id);
	  if (rowid>=0) 
	    if (sqlite3_blob_open(rhizome_db,"main","files","data",rowid,0,&r->blob) !=SQLITE_OK)
	      rowid=-1;
	  
	  if (rowid<0) {
	    rhizome_server_simple_http_response(r,404,"<html><h1>Sorry, can't find that here.</h1></html>\r\n");
	    WHY("File not found / blob not opened");
	  }
	  else {
	    r->source_index=0;
	    r->blob_end=sqlite3_blob_bytes(r->blob);
	    rhizome_server_http_response_header(r,200,"application/binary",
						r->blob_end - r->source_index);
	    r->request_type|=RHIZOME_HTTP_REQUEST_BLOB;
	  }
	}
      }
    else if (sscanf(r->request,"GET /rhizome/manifest/%s HTTP/1.", id)==1)
      {
	/* Stream the specified manifest */
	hexFilter(id);
	WHYF("get /rhizome/manifest/ [%s]",id);
	rhizome_server_simple_http_response(r,400,"<html><h1>A specific manifest</h1></html>\r\n");
      }
    else 
      rhizome_server_simple_http_response(r,400,"<html><h1>Sorry, couldn't parse your request.</h1></html>\r\n");
  }
  else 
    rhizome_server_simple_http_response(r,400,"<html><h1>Sorry, your request was too long.</h1></html>\r\n");
  
  /* Try sending data immediately. */
  rhizome_server_http_send_bytes(rn,r);

  return 0;
}


/* Return appropriate message for HTTP response codes, both known and unknown. */
#define A_VALUE_GREATER_THAN_FOUR (2+3)
char *httpResultString(int id) {
  switch (id) {
  case 200: return "OK"; break;
  case 206: return "Partial Content"; break;
  case 404: return "Not found"; break;
  default: 
  case A_VALUE_GREATER_THAN_FOUR:
    if (id>4) return "A suffusion of yellow";
    /* The following MUST be the longest string returned by this function */
    else return "THE JUDGEMENT OF KING WEN: Chun Signifies Difficulties At Outset, As Of Blade Of Grass Pushing Up Against Stone.";
  }
}

int rhizome_server_simple_http_response(rhizome_http_request *r,int result, char *response)
{
  if (r->buffer) free(r->buffer);
  r->buffer_size=strlen(response)+strlen("HTTP/1.0 000 \r\n\r\n")+strlen(httpResultString(A_VALUE_GREATER_THAN_FOUR))+100;

  r->buffer=(unsigned char *)malloc(r->buffer_size);
  snprintf((char *)r->buffer,r->buffer_size,"HTTP/1.0 %03d %s\r\nContent-type: text/html\r\nContent-length: %lld\r\n\r\n%s",result,httpResultString(result),(int)strlen(response),response);
  
  r->buffer_size=strlen((char *)r->buffer)+1;
  r->buffer_length=r->buffer_size-1;
  r->buffer_offset=0;

  r->request_type=RHIZOME_HTTP_REQUEST_FROMBUFFER;
  return 0;
}

/*
  return codes:
  1: connection still open.
  0: connection finished.
  <0: an error occurred.
*/
int rhizome_server_http_send_bytes(int rn,rhizome_http_request *r)
{
  if (debug&DEBUG_RHIZOME) WHYF("Request #%d, type=0x%x\n",rn,r->request_type);

  // keep writing until the write would block or we run out of data
  while(r->request_type){
    
    /* Flush anything out of the buffer if present, before doing any further
       processing */
    if (r->request_type&RHIZOME_HTTP_REQUEST_FROMBUFFER)
      {
	int bytes=r->buffer_length-r->buffer_offset;
	bytes=write(r->socket,&r->buffer[r->buffer_offset],bytes);
	if (bytes<=0){
	  // stop writing when the tcp buffer is full
	  // TODO errors?
	  return 1;
	}
	
	if (0)
	  dump("bytes written",&r->buffer[r->buffer_offset],bytes);
	r->buffer_offset+=bytes;
	  
	if (r->buffer_offset>=r->buffer_length) {
	  /* Buffer's cleared */
	  r->request_type&=~RHIZOME_HTTP_REQUEST_FROMBUFFER;
	  r->buffer_offset=0; r->buffer_length=0;
	}
	
	// go around the loop again to work out what we should do next
	continue;
	
      }

    switch(r->request_type&(~RHIZOME_HTTP_REQUEST_FROMBUFFER))
      {
      case RHIZOME_HTTP_REQUEST_FAVICON:
	if (r->buffer_size<favicon_len) {
	  free(r->buffer);
	  r->buffer_size=0;
	  r->buffer=malloc(favicon_len);
	  if (!r->buffer) r->request_type=0;
	}
	if (r->buffer)
	{
	    int i;
	    for(i=0;i<favicon_len;i++)
	      r->buffer[i]=favicon_bytes[i];
	    r->buffer_length=i;
	    printf("buffer_length for favicon is %d\n",r->buffer_length);
	    r->request_type=RHIZOME_HTTP_REQUEST_FROMBUFFER;
	}
	
	break;
      case RHIZOME_HTTP_REQUEST_BLOB:
	{
	  /* Get more data from the file and put it in the buffer */
	  int read_size = 65536;
	  if (r->blob_end-r->source_index < read_size)
	    read_size = r->blob_end-r->source_index;
	    
	  r->request_type=0;
	  if (read_size>0){
	    
	    if (r->buffer_size < read_size) {
	      if (r->buffer)
		free(r->buffer);
	      r->buffer=malloc(read_size);
	      if (!r->buffer) {
		if (debug&DEBUG_RHIZOME) WHY("malloc() failed");
		r->request_type=0; break;
	      }
	      r->buffer_size=read_size;
	    }
	      
	    if(sqlite3_blob_read(r->blob,&r->buffer[0],read_size,r->source_index)
	       ==SQLITE_OK)
	      {
		r->buffer_length = read_size;
		r->source_index+=read_size;
		r->request_type|=RHIZOME_HTTP_REQUEST_FROMBUFFER;
	      }
	  }
	    
	  if (r->source_index >= r->blob_end){
	    sqlite3_blob_close(r->blob);
	    r->blob=0;
	  }else
	    r->request_type|=RHIZOME_HTTP_REQUEST_BLOB;
	}
	break;
	  
      default:
	WHY("sending data from this type of HTTP request not implemented");
	r->request_type=0;
	break;
      }
  }
  if (!r->request_type) return rhizome_server_close_http_request(rn);	  
  return 1;
}

int rhizome_server_http_response_header(rhizome_http_request *r,int result,
					char *mime_type,unsigned long long bytes)
{
  int min_buff = strlen("HTTP/1.0 000 \r\nContent-type: \r\nContent-length: \r\n\r\n")
    +strlen(httpResultString(result))
    +strlen(mime_type)+20;
  
  if (min_buff+bytes > 65536){
    min_buff = 65536;
  }else{
    min_buff += bytes;
  }
  
  if (r->buffer_size < min_buff) {
    if (r->buffer)
      free(r->buffer);
    r->buffer=(unsigned char *)malloc(min_buff);
    r->buffer_size=min_buff;
  }
  
  snprintf((char *)r->buffer,r->buffer_size,"HTTP/1.0 %03d %s\r\nContent-type: %s\r\nContent-length: %lld\r\n\r\n",result,httpResultString(result),mime_type,bytes);
  
  r->buffer_length=strlen((char *)r->buffer);
  r->buffer_offset=0;

  r->request_type|=RHIZOME_HTTP_REQUEST_FROMBUFFER;
  return 0;
}
	    
